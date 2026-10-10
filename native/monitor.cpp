#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include "stats-reader.hpp"

using json=nlohmann::json;
namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;
constexpr INTERNET_PORT PORT=9335;
std::atomic<bool> stopRequested{false};
BOOL WINAPI consoleStop(DWORD){stopRequested=true;return TRUE;}

std::wstring wide(const std::string& value){
  if(value.empty())return {};
  const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);
  if(!n)throw std::runtime_error("Invalid UTF-8 path");
  std::wstring out(n,0);MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),out.data(),n);return out;
}
fs::path environmentPath(const wchar_t* key){const auto v=_wgetenv(key);return v&&*v?fs::path(v):fs::path();}
fs::path executableFolder(){std::wstring out(32768,0);auto n=GetModuleFileNameW(nullptr,out.data(),static_cast<DWORD>(out.size()));if(!n)throw std::runtime_error("Executable path unavailable");out.resize(n);return fs::path(out).parent_path();}
std::string readText(const fs::path& file){std::ifstream in(file,std::ios::binary);if(!in)throw std::runtime_error("Monitor source missing");return {std::istreambuf_iterator<char>(in),{}};}
struct HttpHandle{
  HINTERNET value=nullptr;
  explicit HttpHandle(HINTERNET h=nullptr):value(h){}
  ~HttpHandle(){if(value)WinHttpCloseHandle(value);}
  HttpHandle(const HttpHandle&)=delete;HttpHandle& operator=(const HttpHandle&)=delete;
};
HINTERNET sessionHandle(){
  auto h=WinHttpOpen(L"CodexSessionMonitor/2",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
  if(!h)throw std::runtime_error("Local connection unavailable");
  WinHttpSetTimeouts(h,1000,1500,3000,3000);return h;
}
json localTargets(){
  HttpHandle session(sessionHandle()),connection(WinHttpConnect(session.value,L"127.0.0.1",PORT,0));
  HttpHandle request(WinHttpOpenRequest(connection.value,L"GET",L"/json/list",nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,0));
  DWORD disabled=WINHTTP_DISABLE_REDIRECTS;WinHttpSetOption(request.value,WINHTTP_OPTION_DISABLE_FEATURE,&disabled,sizeof(disabled));
  if(!request.value||!WinHttpSendRequest(request.value,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)||!WinHttpReceiveResponse(request.value,nullptr))throw std::runtime_error("Local debugger not available");
  DWORD status=0,length=sizeof(status);WinHttpQueryHeaders(request.value,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&length,nullptr);
  if(status!=200)throw std::runtime_error("Unexpected local response");
  std::string body;char buffer[8192];DWORD received=0;
  while(WinHttpReadData(request.value,buffer,sizeof(buffer),&received)&&received){body.append(buffer,received);if(body.size()>524288)throw std::runtime_error("Target list exceeds limit");}
  auto result=json::parse(body,nullptr,false);if(!result.is_array())throw std::runtime_error("Invalid target list");return result;
}
bool eligible(const json& target){return target.is_object()&&target.value("type","")=="page"&&target.value("url","")=="app://-/index.html";}

class Cdp{
  HttpHandle session,connection;
  std::atomic<HINTERNET> socket{nullptr};
  std::atomic<bool> alive{false};
  std::thread receiver;
  std::mutex lock,sendLock;
  std::condition_variable changed;
  std::unordered_map<unsigned,json> replies;
  std::deque<std::string> bindings;
  unsigned nextId=0;
  void receiveLoop(){
    try{
    std::string message;char buffer[8192];
    while(alive){
      DWORD count=0;WINHTTP_WEB_SOCKET_BUFFER_TYPE kind;
      auto error=WinHttpWebSocketReceive(socket.load(),buffer,sizeof(buffer),&count,&kind);
      if(error||kind==WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)break;
      message.append(buffer,count);if(message.size()>524288)break;
      if(kind!=WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE&&kind!=WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)continue;
      auto value=json::parse(message,nullptr,false);message.clear();
      if(!value.is_object())continue;
      std::lock_guard<std::mutex> guard(lock);
      if(value.contains("id")&&value["id"].is_number_unsigned()){
        const auto replyId=value["id"].get<unsigned>();
        if(replies.size()<32)replies[replyId]=std::move(value);
      }
      else if(value.value("method","")=="Runtime.bindingCalled"){
        auto p=value.value("params",json::object());
        if(p.value("name","")=="__codexSessionMonitorRequest"&&p.contains("payload")&&p["payload"].is_string()){
          // Keep only the newest selection when a large first log read is in progress.
          bindings.clear();bindings.push_back(p["payload"].get<std::string>());
        }
      }
      changed.notify_all();
    }
    }catch(const std::exception&){ }
    alive=false;changed.notify_all();
  }
public:
  explicit Cdp(const json& target):session(sessionHandle()),connection(WinHttpConnect(session.value,L"127.0.0.1",PORT,0)){
    const std::string address=target.value("webSocketDebuggerUrl","");
    const std::string prefix="ws://127.0.0.1:9335/devtools/page/";
    if(address.rfind(prefix,0)!=0||address.find_first_of("?#@\\",prefix.size())!=std::string::npos)throw std::runtime_error("Only the local app debugger is allowed");
    HttpHandle request(WinHttpOpenRequest(connection.value,L"GET",wide(address.substr(std::string("ws://127.0.0.1:9335").size())).c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,0));
    DWORD disabled=WINHTTP_DISABLE_REDIRECTS;WinHttpSetOption(request.value,WINHTTP_OPTION_DISABLE_FEATURE,&disabled,sizeof(disabled));
    if(!request.value||!WinHttpSetOption(request.value,WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,nullptr,0)||!WinHttpSendRequest(request.value,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)||!WinHttpReceiveResponse(request.value,nullptr))throw std::runtime_error("Debugger connection failed");
    auto ws=WinHttpWebSocketCompleteUpgrade(request.value,0);if(!ws)throw std::runtime_error("Debugger upgrade failed");
    DWORD closeTimeout=1000;WinHttpSetOption(ws,WINHTTP_OPTION_WEB_SOCKET_CLOSE_TIMEOUT,&closeTimeout,sizeof(closeTimeout));
    socket=ws;alive=true;receiver=std::thread([this]{receiveLoop();});
  }
  ~Cdp(){alive=false;auto ws=socket.exchange(nullptr);if(ws)WinHttpCloseHandle(ws);changed.notify_all();if(receiver.joinable())receiver.join();}
  bool connected()const{return alive;}
  json call(const std::string& method,const json& params=json::object()){
    const unsigned id=++nextId;auto message=json{{"id",id},{"method",method},{"params",params}}.dump();
    {std::lock_guard<std::mutex> guard(sendLock);if(!alive||WinHttpWebSocketSend(socket.load(),WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,message.data(),static_cast<DWORD>(message.size()))!=NO_ERROR)throw std::runtime_error("Debugger disconnected");}
    std::unique_lock<std::mutex> guard(lock);
    if(!changed.wait_for(guard,std::chrono::seconds(5),[&]{return !alive||replies.count(id);})||!replies.count(id))throw std::runtime_error("Debugger request timed out");
    auto result=std::move(replies[id]);replies.erase(id);if(result.contains("error"))throw std::runtime_error("Debugger command rejected");return result.value("result",json::object());
  }
  json evaluate(const std::string& expression){
    auto result=call("Runtime.evaluate",{{"expression",expression},{"returnByValue",true},{"awaitPromise",true}});
    if(result.contains("exceptionDetails"))throw std::runtime_error("Statistics UI failed");
    return result.value("result",json::object()).value("value",json());
  }
  std::string waitBinding(std::chrono::milliseconds timeout){
    std::unique_lock<std::mutex> guard(lock);changed.wait_for(guard,timeout,[&]{return !alive||!bindings.empty()||stopRequested;});
    if(bindings.empty())return {};
    auto value=std::move(bindings.front());bindings.pop_front();return value;
  }
};
class StateWriter{
  fs::path folder;std::string previous;
public:
  explicit StateWriter(fs::path value):folder(std::move(value)){fs::create_directories(folder);}
  void write(const std::string& phase,const std::string& threadId="",const json& health=json()){
    json value={{"pid",GetCurrentProcessId()},{"port",PORT},{"backend","native"},{"phase",phase}};
    if(!threadId.empty())value["threadId"]=threadId;
    if(!health.is_null())value["health"]=health;
    auto fingerprint=value.dump();if(fingerprint==previous)return;
    value["atEpochMs"]=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    auto temporary=folder/("status-"+std::to_string(GetCurrentProcessId())+".tmp");
    {std::ofstream output(temporary,std::ios::binary);output<<value.dump(2);if(!output)throw std::runtime_error("Could not write monitor state");}
    if(!MoveFileExW(temporary.c_str(),(folder/L"status.json").c_str(),MOVEFILE_REPLACE_EXISTING))throw std::runtime_error("Could not replace monitor state");
    previous=std::move(fingerprint);
  }
};
bool validThread(const std::string& id){if(id.size()!=36)return false;for(size_t i=0;i<id.size();++i){if(i==8||i==13||i==18||i==23){if(id[i]!='-')return false;}else if(!isxdigit(static_cast<unsigned char>(id[i])))return false;}return true;}

int main(int argc,char** argv){
  SetConsoleCtrlHandler(consoleStop,TRUE);
  try{
    auto codexHome=environmentPath(L"CODEX_HOME");if(codexHome.empty())codexHome=environmentPath(L"USERPROFILE")/L".codex";
    SessionStatsReader stats(codexHome);
    if(argc==3&&std::string(argv[1])=="--stats"){if(!validThread(argv[2]))throw std::runtime_error("Invalid conversation ID");std::cout<<stats.read(argv[2]).dump()<<std::endl;return 0;}
    const bool remove=argc==2&&std::string(argv[1])=="--remove";
    if(argc>1&&!remove)throw std::runtime_error("Unknown monitor argument");
    HANDLE singleton=nullptr;
    if(!remove){singleton=CreateMutexW(nullptr,FALSE,L"Local\\CodexSessionMonitorNative");if(!singleton||GetLastError()==ERROR_ALREADY_EXISTS){if(singleton)CloseHandle(singleton);return 0;}}
    auto stateHome=environmentPath(L"LOCALAPPDATA");if(stateHome.empty())stateHome=fs::temp_directory_path();
    StateWriter state(stateHome/L"CodexSessionMonitor");
    const auto source=remove?std::string():readText(executableFolder()/L"session-ui.js");
    if(!remove)state.write("waiting");
    unsigned absence=0;
    while(!stopRequested){
      try{
        auto targets=localTargets();json target;
        for(const auto& item:targets)if(eligible(item)){target=item;break;}
        if(target.is_null())throw std::runtime_error("Main conversation window not mounted");
        Cdp cdp(target);cdp.call("Runtime.enable");
        if(remove){cdp.evaluate("window.__CODEX_SESSION_ONLY_MONITOR__?.remove()");return 0;}
        cdp.call("Runtime.addBinding",{{"name","__codexSessionMonitorRequest"}});cdp.evaluate(source);absence=0;
        std::string previousStats,lastThread;auto nextHealth=Clock::now()+std::chrono::seconds(15);
        while(cdp.connected()&&!stopRequested){
          const auto wait=std::chrono::duration_cast<std::chrono::milliseconds>(nextHealth-Clock::now());
          auto payload=cdp.waitBinding(wait.count()>0?wait:std::chrono::milliseconds(0));
          if(!payload.empty()){
            auto request=json::parse(payload,nullptr,false);
            const auto id=request.is_object()?request.value("threadId",""):std::string();
            if(validThread(id)){
              auto value=stats.read(id);
              if(value.value("status","")=="unavailable")value["error"]="rollout_unavailable";
              auto fingerprint=value.dump();
              if(request.value("needsSnapshot",false)||fingerprint!=previousStats){
                auto accepted=cdp.evaluate("window.__CODEX_SESSION_ONLY_MONITOR__?.update("+fingerprint+")");
                if(accepted.is_boolean()&&accepted.get<bool>()){previousStats=std::move(fingerprint);lastThread=id;state.write("ready",id,{{"installed",true},{"metrics",8},{"threadId",id}});}
              }
            }
          }
          if(Clock::now()>=nextHealth){
            auto health=cdp.evaluate("window.__CODEX_SESSION_ONLY_MONITOR__?.health()");
            if(!health.is_object()||!health.value("installed",false)){cdp.evaluate(source);previousStats.clear();}
            else if(!health.value("visible",false))state.write("hidden","",health);
            else if(!lastThread.empty())state.write("ready",lastThread,{{"installed",true},{"metrics",8},{"threadId",lastThread}});
            nextHealth=Clock::now()+std::chrono::seconds(15);
          }
        }
      }catch(const std::exception& error){
        if(remove){std::cerr<<error.what()<<std::endl;return 1;}
        state.write("waiting-for-ui");if(++absence>=12)break;
      }
      if(!stopRequested)std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    if(singleton)CloseHandle(singleton);
    return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<std::endl;return 1;}
}
