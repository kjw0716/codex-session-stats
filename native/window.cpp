#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#include "stats-reader.hpp"
#include "floating-ui.hpp"
#include <commctrl.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <future>
#include <iomanip>
#include <sstream>

using json = nlohmann::json;
namespace fs = std::filesystem;
static HWND mainWindow, floatWindow, modeBox, recentBox, stateLabel, autoBox;
static HFONT font, boldFont;
static HFONT smallFont,valueFont,stripFont;
static HWND opacitySlider,opacityValue,floatTooltip;
static json latestStats=json::object();
static std::wstring tooltipText;
static HBRUSH floatBrush;
static json prefs = json::object();
static bool autoEnabled=false;
static constexpr COLORREF background=RGB(23,25,31), foreground=RGB(232,234,241), muted=RGB(156,163,181), accent=RGB(116,105,255);
static void themeWindow(HWND h){
    HMODULE lib=LoadLibraryW(L"dwmapi.dll");if(!lib)return;
    using SetAttribute=HRESULT(WINAPI*)(HWND,DWORD,LPCVOID,DWORD);
    auto set=reinterpret_cast<SetAttribute>(GetProcAddress(lib,"DwmSetWindowAttribute"));
    if(set){BOOL enabled=TRUE;set(h,20,&enabled,sizeof(enabled));COLORREF color=background;set(h,35,&color,sizeof(color));}
    FreeLibrary(lib);
}
static LRESULT drawControl(DRAWITEMSTRUCT* d){
    HDC dc=d->hDC;RECT r=d->rcItem;SetBkMode(dc,TRANSPARENT);SelectObject(dc,font);
    if(d->CtlType==ODT_COMBOBOX){
        HBRUSH brush=CreateSolidBrush(d->itemState&ODS_SELECTED?RGB(55,52,85):RGB(35,38,47));FillRect(dc,&r,brush);DeleteObject(brush);
        if(d->itemID!=UINT(-1)){wchar_t text[1024]{};SendMessageW(d->hwndItem,CB_GETLBTEXT,d->itemID,(LPARAM)text);r.left+=10;r.right-=8;SetTextColor(dc,foreground);DrawTextW(dc,text,-1,&r,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);}
        return TRUE;
    }
    bool toggle=d->CtlID==13||d->CtlID==15;
    bool checked=d->CtlID==13?autoEnabled:prefs.value("floatOnTop",true);
    COLORREF color=d->CtlID==11?accent:RGB(39,43,54);
    if(d->itemState&ODS_SELECTED)color=RGB(73,69,112);
    FillRect(dc,&r,floatBrush);
    HBRUSH brush=CreateSolidBrush(toggle?background:color);HPEN pen=CreatePen(PS_SOLID,1,toggle?background:RGB(63,67,82));auto oldB=SelectObject(dc,brush);auto oldP=SelectObject(dc,pen);
    RoundRect(dc,r.left,r.top,r.right,r.bottom,10,10);SelectObject(dc,oldB);SelectObject(dc,oldP);DeleteObject(brush);DeleteObject(pen);
    if(toggle){
        RECT track={r.left+4,(r.bottom+r.top)/2-8,r.left+34,(r.bottom+r.top)/2+8};
        brush=CreateSolidBrush(checked?accent:RGB(65,70,83));oldB=SelectObject(dc,brush);oldP=SelectObject(dc,GetStockObject(NULL_PEN));RoundRect(dc,track.left,track.top,track.right,track.bottom,16,16);SelectObject(dc,oldB);DeleteObject(brush);
        brush=CreateSolidBrush(foreground);oldB=SelectObject(dc,brush);int x=checked?track.right-14:track.left+2;Ellipse(dc,x,track.top+2,x+12,track.top+14);SelectObject(dc,oldB);SelectObject(dc,oldP);DeleteObject(brush);r.left+=45;
    }
    wchar_t text[256]{};GetWindowTextW(d->hwndItem,text,256);SetTextColor(dc,foreground);DrawTextW(dc,text,-1,&r,DT_SINGLELINE|DT_VCENTER|(toggle?DT_LEFT:DT_CENTER)|DT_END_ELLIPSIS|DT_NOPREFIX);
    if(d->itemState&ODS_FOCUS){InflateRect(&r,-3,-3);DrawFocusRect(dc,&r);}return TRUE;
}
static fs::path root, home, stateRoot, resultPath;
static std::vector<session_monitor_detail::SystemSqlite::Recent> recent;
static std::unique_ptr<SessionStatsReader> reader;
static std::future<json> pendingRead;
static HANDLE launchProcess = nullptr;
static std::wstring operation;
static bool waiting = false, floating = false;
static bool shuttingDown = false;
static int smokePhase = 0;
static json smokeChecks = json::object();
static std::string selected;
static ULONGLONG readAfter = 0;
static fs::path smokePath, previewPath;
static void capturePreview(HWND target, const fs::path& output) {
    RECT r{}; GetWindowRect(target,&r);r.right-=r.left;r.bottom-=r.top;r.left=r.top=0;
    HDC dc=GetDC(target), memory=CreateCompatibleDC(dc);
    BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=r.right;bi.bmiHeader.biHeight=r.bottom;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
    void* pixels=nullptr;HBITMAP bitmap=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&pixels,nullptr,0);HGDIOBJ old=SelectObject(memory,bitmap);
    RECT fill=r;FillRect(memory,&fill,(HBRUSH)(COLOR_WINDOW+1));
    SendMessageW(target,WM_PRINT,(WPARAM)memory,PRF_CLIENT|PRF_CHILDREN|PRF_ERASEBKGND|PRF_NONCLIENT);
    BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);header.bfSize=header.bfOffBits+r.right*r.bottom*4;
    std::ofstream out(output,std::ios::binary);out.write((char*)&header,sizeof(header));out.write((char*)&bi.bmiHeader,sizeof(BITMAPINFOHEADER));out.write((char*)pixels,r.right*r.bottom*4);
    SelectObject(memory,old);DeleteObject(bitmap);DeleteDC(memory);ReleaseDC(target,dc);
}
static std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n); return out;
}
static std::string textFile(const fs::path& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
static void status(const std::wstring& s) { SetWindowTextW(stateLabel, s.c_str()); }
static void savePrefs() { std::ofstream(stateRoot / L"preferences.json") << prefs.dump(2); }
static HWND control(const wchar_t* cls, const wchar_t* label, DWORD style, int x, int y, int w, int h, int id, HWND parent=nullptr) {
    HWND c = CreateWindowW(cls, label, WS_CHILD | WS_VISIBLE | style, x,y,w,h,parent?parent:mainWindow,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);
    if(std::wstring(cls)==L"COMBOBOX"){
        HMODULE theme=LoadLibraryW(L"uxtheme.dll");
        if(theme){using SetTheme=HRESULT(WINAPI*)(HWND,LPCWSTR,LPCWSTR);auto set=reinterpret_cast<SetTheme>(GetProcAddress(theme,"SetWindowTheme"));if(set)set(c,L"DarkMode_CFD",nullptr);FreeLibrary(theme);}
    }
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE); return c;
}
static bool appRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if (snap == INVALID_HANDLE_VALUE) return true;
    PROCESSENTRY32W p{}; p.dwSize = sizeof(p); bool found = false;
    if (Process32FirstW(snap,&p)) do {
        if (_wcsicmp(p.szExeFile,L"ChatGPT.exe")) continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,p.th32ProcessID);
        if (!process) { found = true; break; }
        wchar_t path[32768]; DWORD size=32768;
        if (QueryFullProcessImageNameW(process,0,path,&size)) {
            std::wstring s(path,size);
            if (s.find(L"\\WindowsApps\\OpenAI.Codex_") != std::wstring::npos && s.size() >= 16 && s.substr(s.size()-16) == L"\\app\\ChatGPT.exe") found=true;
        }
        CloseHandle(process); if(found) break;
    } while(Process32NextW(snap,&p));
    CloseHandle(snap); return found;
}
static void runScript(const wchar_t* name, const std::wstring& args=L"") {
    if (launchProcess) { status(L"A startup/settings operation is still running."); return; }
    std::error_code ec; fs::remove(resultPath, ec);
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys,MAX_PATH);
    fs::path exe=fs::path(sys)/L"WindowsPowerShell/v1.0/powershell.exe";
    std::wstring command=L"\""+exe.wstring()+L"\" -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \""+(root/name).wstring()+L"\" "+args;
    STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
    if(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,root.c_str(),&si,&pi)) {
        operation=name;launchProcess=pi.hProcess;CloseHandle(pi.hThread);status(L"Working...");
    } else status(L"Could not start the launcher. Check the installation files.");
}
static void startEmbedded() {
    if(launchProcess){status(L"A startup/settings operation is still running.");return;}
    waiting=false;
    if(floatWindow)DestroyWindow(floatWindow);
    runScript(L"Start-EmbeddedStats.ps1",L"-Quiet -PanelResult \""+resultPath.wstring()+L"\"");
}
static void refreshRecent() {
    std::string previous=selected;
    session_monitor_detail::SystemSqlite db; recent=db.recent(home);
    SendMessageW(recentBox,CB_RESETCONTENT,0,0); int chosen=0;
    for(size_t i=0;i<recent.size();++i) {
        std::wstring label=wide(recent[i].title)+L"  ["+wide(recent[i].id.substr(0,8))+L"]";
        SendMessageW(recentBox,CB_ADDSTRING,0,(LPARAM)label.c_str()); if(recent[i].id==selected) chosen=int(i);
    }
    if(!recent.empty()) {SendMessageW(recentBox,CB_SETCURSEL,chosen,0);selected=recent[chosen].id;}
    else {selected.clear();status(L"No local conversations found");}
    if(previous!=selected){latestStats=json::object();if(floatWindow)InvalidateRect(floatWindow,nullptr,FALSE);}
}
static LRESULT CALLBACK floatProc(HWND,UINT,WPARAM,LPARAM);
static void showDashboard() { ShowWindow(mainWindow,SW_RESTORE);SetForegroundWindow(mainWindow); }
static void layoutFloating(HWND);
static void applyOpacity();
static void setFloatingSize(int,int);
static void openFloating() {
    if(launchProcess){status(L"Wait for the current startup/settings operation to finish.");return;}
    waiting=false;
    if(!floatWindow) {
        RECT dashboard{};GetWindowRect(mainWindow,&dashboard);
        MONITORINFO display{};display.cbSize=sizeof(display);GetMonitorInfoW(MonitorFromWindow(mainWindow,MONITOR_DEFAULTTONEAREST),&display);
        int width=std::clamp(prefs.value("floatWidth",460),220,int(display.rcWork.right-display.rcWork.left));
        int height=std::clamp(prefs.value("floatHeight",48),40,int(display.rcWork.bottom-display.rcWork.top));
        int x=(std::max)(display.rcWork.left,(std::min)(dashboard.right+16,display.rcWork.right-width));
        int y=(std::max)(display.rcWork.top,(std::min)(dashboard.top,display.rcWork.bottom-height));
        // An independent top-level window: no owner, so hiding/minimizing the dashboard cannot hide it.
        CreateWindowExW(WS_EX_APPWINDOW|WS_EX_LAYERED,L"CodexSessionStatsFloating",L"Session Stats - Floating",WS_POPUP|WS_THICKFRAME|WS_SYSMENU|WS_MINIMIZEBOX,x,y,width,height,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    }
    floating=true;readAfter=0;
    if(smokePath.empty()){ShowWindow(floatWindow,SW_RESTORE);SetForegroundWindow(floatWindow);}
    status(L"Floating window open");
}
static void tick() {
    if(launchProcess && WaitForSingleObject(launchProcess,0)==WAIT_OBJECT_0) {
        DWORD exitCode=0;GetExitCodeProcess(launchProcess,&exitCode);
        CloseHandle(launchProcess);launchProcess=nullptr;
        std::string result=textFile(resultPath);
        if(exitCode!=0)status(L"The operation failed. Check the installation files and launcher.log.\nChatGPT was not closed.");
        else if(result.find("Quit Codex/ChatGPT normally")!=std::string::npos) {
            waiting=true;status(L"Waiting for ChatGPT to exit...");
        } else if(result.find("Session Stats started")!=std::string::npos || result.find("already running")!=std::string::npos) status(L"Inside conversation: connected");
        else if(result.find("error:")!=std::string::npos) status(wide(result));
        else if(!result.empty())status(wide(result));
        else status(operation==L"Stop-EmbeddedStats.ps1"?L"Stopped":L"Startup setting saved");
    }
    if(waiting && !launchProcess && !appRunning()) startEmbedded();
    if(pendingRead.valid() && pendingRead.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        try {
            json j=pendingRead.get();
            if(floating && j.value("threadId",std::string())==selected) {
                if(latestStats!=j){latestStats=j;InvalidateRect(floatWindow,nullptr,FALSE);}
                if(!smokePath.empty()) {
                    if(smokePhase==0){
                        smokeChecks["separateWindows"]=floatWindow!=mainWindow && GetAncestor(recentBox,GA_ROOT)==floatWindow && GetWindow(floatWindow,GW_OWNER)==nullptr;
                        smokeChecks["dashboardHasNoMetrics"]=GetDlgItem(mainWindow,100)==nullptr && GetDlgItem(mainWindow,16)==nullptr;
                        smokeChecks["dashboardResizable"]=(GetWindowLongPtrW(mainWindow,GWL_STYLE)&WS_THICKFRAME)!=0;
                        SetWindowPos(mainWindow,nullptr,0,0,720,620,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
                        RECT large{};GetWindowRect(modeBox,&large);
                        SetWindowPos(mainWindow,nullptr,0,0,420,570,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
                        RECT small{},client{},footer{};GetWindowRect(modeBox,&small);GetClientRect(mainWindow,&client);GetWindowRect(stateLabel,&footer);MapWindowPoints(nullptr,mainWindow,(POINT*)&footer,2);
                        smokeChecks["dashboardLayoutResizes"]=(large.right-large.left)>(small.right-small.left) && footer.bottom<=client.bottom && footer.right<=client.right;
                        json adaptive=json::object();bool layoutsFit=true;
                        for(int width:{220,260,329,330,460,510,620,900})for(int height:{40,48,77,78,117,118,150,183,184,251,252,288,500}){
                            auto layout=floating_ui::measure(width,height);
                            for(size_t a=0;a<layout.cells.size();++a){auto r=layout.cells[a].rect;layoutsFit&=r.left>=0&&r.top>=0&&r.right<=width&&r.bottom<=height&&r.left<r.right&&r.top<r.bottom;RECT overlap{};layoutsFit&=!IntersectRect(&overlap,&r,&layout.menu);for(size_t b=a+1;b<layout.cells.size();++b)layoutsFit&=!IntersectRect(&overlap,&r,&layout.cells[b].rect);}
                        }
                        adaptive["layoutsFit"]=layoutsFit;
                        SetWindowPos(floatWindow,nullptr,0,0,220,40,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
                        RECT widget{};GetClientRect(floatWindow,&widget);auto mini=floating_ui::measure(widget.right,widget.bottom);
                        adaptive["singleLineMinimum"]=widget.right==220&&widget.bottom==40&&mini.strip&&mini.cells.size()==1&&mini.cells[0].label==L"Tokens";
                        adaptive["noTitleOrFooter"]=GetDlgItem(floatWindow,120)==nullptr&&GetDlgItem(floatWindow,15)==nullptr&&GetDlgItem(floatWindow,18)==nullptr&&(GetWindowLongPtrW(floatWindow,GWL_STYLE)&WS_CAPTION)==0;
                        adaptive["pickerHiddenInMini"]=(GetWindowLongPtrW(recentBox,GWL_STYLE)&WS_VISIBLE)==0;
                        POINT edge={widget.right-2,widget.bottom/2};ClientToScreen(floatWindow,&edge);adaptive["edgeResizeWorks"]=SendMessageW(floatWindow,WM_NCHITTEST,0,MAKELPARAM(edge.x,edge.y))==HTRIGHT;
                        SetWindowPos(floatWindow,nullptr,0,0,460,288,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);adaptive["detailsRestore"]=floating_ui::measure(460,288).cells.size()==6&&(GetWindowLongPtrW(recentBox,GWL_STYLE)&WS_VISIBLE)!=0;
                        SendMessageW(opacitySlider,TBM_SETPOS,TRUE,35);SendMessageW(mainWindow,WM_HSCROLL,TB_THUMBPOSITION,(LPARAM)opacitySlider);
                        BYTE alpha=0;DWORD flags=0;GetLayeredWindowAttributes(floatWindow,nullptr,&alpha,&flags);adaptive["opacityApplied"]=alpha==89&&(flags&LWA_ALPHA);
                        smokeChecks["adaptiveFloating"]=adaptive;
                        SendMessageW(mainWindow,WM_CLOSE,0,0);
                        smokeChecks["floatingSurvivesDashboardClose"]=IsWindow(floatWindow)!=0 && floating && !IsWindowVisible(mainWindow);
                        smokePhase=1;readAfter=0;return;
                    }
                    j["recentCount"]=recent.size();j["nativePanel"]=true;j["windowChecks"]=smokeChecks;
                    j["updatesWithDashboardClosed"]=!IsWindowVisible(mainWindow) && IsWindow(floatWindow);
                    DestroyWindow(floatWindow);
                    j["floatingClosesCleanly"]=!floating && !floatWindow;
                    std::ofstream(smokePath)<<j.dump(2);PostMessageW(mainWindow,WM_APP+3,0,0);
                }
                readAfter=GetTickCount64()+(j.value("status",std::string())=="running"?2500:10000);
            }
        } catch(...) {status(L"Could not read the selected conversation");readAfter=GetTickCount64()+10000;}
    }
    if(floating && floatWindow && !IsIconic(floatWindow) && !pendingRead.valid() && !selected.empty() && GetTickCount64()>=readAfter) {
        const std::string id=selected;pendingRead=std::async(std::launch::async,[id]{return reader->read(id);});
    }
}
static void applyOpacity(){
    int percent=std::clamp(prefs.value("floatOpacity",94),35,100);
    if(floatWindow)SetLayeredWindowAttributes(floatWindow,0,BYTE((percent*255+50)/100),LWA_ALPHA);
    if(opacityValue)SetWindowTextW(opacityValue,(std::to_wstring(percent)+L"%").c_str());
}
static void layoutFloating(HWND h){
    RECT r{};GetClientRect(h,&r);auto layout=floating_ui::measure(r.right,r.bottom);
    ShowWindow(recentBox,layout.picker?SW_SHOW:SW_HIDE);
    if(layout.picker)MoveWindow(recentBox,10,10,r.right-54,250,TRUE);
    SetWindowRgn(h,CreateRoundRectRgn(0,0,r.right+1,r.bottom+1,14,14),TRUE);
    InvalidateRect(h,nullptr,FALSE);
}
static void setFloatingSize(int width,int height){
    if(!floatWindow)openFloating();if(!floatWindow)return;
    SetWindowPos(floatWindow,nullptr,0,0,width,height,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    prefs["floatWidth"]=width;prefs["floatHeight"]=height;savePrefs();
}
static void chooseRecent(int index){
    if(index<0||size_t(index)>=recent.size())return;
    selected=recent[index].id;prefs["threadId"]=selected;savePrefs();readAfter=0;latestStats=json::object();
    SendMessageW(recentBox,CB_SETCURSEL,index,0);InvalidateRect(floatWindow,nullptr,FALSE);
}
static void floatingMenu(HWND h,POINT point){
    HMENU menu=CreatePopupMenu(),sessions=CreatePopupMenu();
    for(size_t i=0;i<recent.size();++i){auto name=wide(recent[i].title);if(name.size()>60)name=name.substr(0,57)+L"...";std::wstring escaped;for(wchar_t c:name){escaped+=c;if(c==L'&')escaped+=c;}
        AppendMenuW(sessions,MF_STRING|(selected==recent[i].id?MF_CHECKED:0),1000+i,escaped.c_str());}
    AppendMenuW(menu,MF_POPUP,(UINT_PTR)sessions,L"Conversation");
    AppendMenuW(menu,MF_STRING,17,L"Refresh");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    AppendMenuW(menu,MF_STRING,31,L"Mini");AppendMenuW(menu,MF_STRING,32,L"Compact");AppendMenuW(menu,MF_STRING,33,L"Details");
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING|(prefs.value("floatOnTop",true)?MF_CHECKED:0),15,L"Pin window");
    AppendMenuW(menu,MF_STRING,18,L"Dashboard");AppendMenuW(menu,MF_STRING,19,L"Close");
    UINT choice=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,h,nullptr);DestroyMenu(menu);
    if(choice>=1000)chooseRecent(int(choice-1000));
    else if(choice==17){refreshRecent();readAfter=0;}
    else if(choice==18)showDashboard();
    else if(choice==19)PostMessageW(h,WM_CLOSE,0,0);
    else if(choice==31)setFloatingSize(460,48);
    else if(choice==32)setFloatingSize(460,150);
    else if(choice==33)setFloatingSize(460,288);
    else if(choice==15){bool top=!prefs.value("floatOnTop",true);prefs["floatOnTop"]=top;savePrefs();SetWindowPos(h,top?HWND_TOPMOST:HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);}
}
static void paintFloating(HWND h,HDC target){
    RECT r{};GetClientRect(h,&r);if(r.right<=0||r.bottom<=0)return;
    HDC memory=CreateCompatibleDC(target);HBITMAP bitmap=CreateCompatibleBitmap(target,r.right,r.bottom);auto old=SelectObject(memory,bitmap);
    floating_ui::paint(memory,r.right,r.bottom,latestStats,font,smallFont,valueFont,stripFont);
    BitBlt(target,0,0,r.right,r.bottom,memory,0,0,SRCCOPY);SelectObject(memory,old);DeleteObject(bitmap);DeleteDC(memory);
}
static LRESULT CALLBACK floatProc(HWND h,UINT m,WPARAM w,LPARAM l) {
    switch(m){
    case WM_CREATE:{
        floatWindow=h;themeWindow(h);
        recentBox=control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_VSCROLL|WS_TABSTOP,10,10,300,250,16,h);
        SendMessageW(recentBox,CB_SETITEMHEIGHT,WPARAM(-1),26);
        SetWindowPos(h,prefs.value("floatOnTop",true)?HWND_TOPMOST:HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        floatTooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,h,nullptr,GetModuleHandleW(nullptr),nullptr);
        TOOLINFOW tool{};tool.cbSize=sizeof(tool);tool.uFlags=TTF_IDISHWND|TTF_SUBCLASS;tool.hwnd=h;tool.uId=(UINT_PTR)h;tool.lpszText=LPSTR_TEXTCALLBACKW;
        SendMessageW(floatTooltip,TTM_ADDTOOLW,0,(LPARAM)&tool);SendMessageW(floatTooltip,TTM_SETMAXTIPWIDTH,0,350);
        refreshRecent();applyOpacity();return 0;
    }
    case WM_NCCALCSIZE:return 0;
    case WM_GETMINMAXINFO:((MINMAXINFO*)l)->ptMinTrackSize={220,40};return 0;
    case WM_NCHITTEST:{
        POINT p={short(LOWORD(l)),short(HIWORD(l))};ScreenToClient(h,&p);RECT r{};GetClientRect(h,&r);
        bool left=p.x<6,right=p.x>=r.right-6,top=p.y<6,bottom=p.y>=r.bottom-6;
        if(top&&left)return HTTOPLEFT;if(top&&right)return HTTOPRIGHT;if(bottom&&left)return HTBOTTOMLEFT;if(bottom&&right)return HTBOTTOMRIGHT;
        if(left)return HTLEFT;if(right)return HTRIGHT;if(top)return HTTOP;if(bottom)return HTBOTTOM;return HTCLIENT;
    }
    case WM_SIZE:if(w!=SIZE_MINIMIZED)layoutFloating(h);return 0;
    case WM_EXITSIZEMOVE:{RECT r{};GetClientRect(h,&r);prefs["floatWidth"]=r.right;prefs["floatHeight"]=r.bottom;savePrefs();return 0;}
    case WM_PAINT:{PAINTSTRUCT ps{};HDC dc=BeginPaint(h,&ps);paintFloating(h,dc);EndPaint(h,&ps);return 0;}
    case WM_PRINTCLIENT:paintFloating(h,(HDC)w);return 0;
    case WM_ERASEBKGND:return 1;
    case WM_LBUTTONDOWN:{
        RECT r{};GetClientRect(h,&r);auto layout=floating_ui::measure(r.right,r.bottom);POINT p={short(LOWORD(l)),short(HIWORD(l))};
        if(PtInRect(&layout.menu,p)){p={layout.menu.left,layout.menu.bottom+3};ClientToScreen(h,&p);floatingMenu(h,p);}
        else{ClientToScreen(h,&p);ReleaseCapture();SendMessageW(h,WM_NCLBUTTONDOWN,HTCAPTION,MAKELPARAM(p.x,p.y));}return 0;
    }
    case WM_CONTEXTMENU:{POINT p={short(LOWORD(l)),short(HIWORD(l))};if(p.x==-1&&p.y==-1)GetCursorPos(&p);floatingMenu(h,p);return 0;}
    case WM_MEASUREITEM:((MEASUREITEMSTRUCT*)l)->itemHeight=28;return TRUE;
    case WM_DRAWITEM:return drawControl((DRAWITEMSTRUCT*)l);
    case WM_CTLCOLORLISTBOX:SetTextColor((HDC)w,foreground);SetBkColor((HDC)w,background);return (LRESULT)floatBrush;
    case WM_COMMAND:if(LOWORD(w)==16&&HIWORD(w)==CBN_SELCHANGE)chooseRecent(int(SendMessageW(recentBox,CB_GETCURSEL,0,0)));return 0;
    case WM_NOTIFY:if(((NMHDR*)l)->code==TTN_GETDISPINFOW){
        tooltipText=latestStats.contains("title")&&latestStats["title"].is_string()?wide(latestStats["title"].get<std::string>()):L"Session Stats";
        tooltipText+=L"\nTokens: "+floating_ui::value(latestStats,0,false)+L"\nOutput: "+floating_ui::value(latestStats,2)+L"\nSpeed includes thinking/tool waits.";
        tooltipText+=L"\nContext: "+floating_ui::value(latestStats,6,false)+L" tokens (latest local record)\nContext: green <70%, yellow 70-<85%, red >=85%.";
        ((NMTTDISPINFOW*)l)->lpszText=tooltipText.data();return 0;
    }break;
    case WM_DESTROY:floating=false;floatWindow=nullptr;recentBox=nullptr;if(floatTooltip)DestroyWindow(floatTooltip);floatTooltip=nullptr;status(L"Floating window closed");if(!shuttingDown&&!IsWindowVisible(mainWindow))PostMessageW(mainWindow,WM_CLOSE,0,0);return 0;
    }return DefWindowProcW(h,m,w,l);
}
static void layoutDashboard(HWND h) {
    RECT r{};GetClientRect(h,&r);int width=r.right,pad=24,gap=12,inner=width-2*pad;
    auto place=[&](int id,int x,int y,int w,int height){MoveWindow(GetDlgItem(h,id),x,y,w,height,TRUE);};
    place(20,pad,22,inner,36);place(21,pad,82,inner,24);
    place(10,pad,112,inner,170);
    int half=(inner-gap)/2;place(11,pad,164,half,40);place(12,pad+half+gap,164,inner-half-gap,40);
    place(36,pad,224,inner,24);
    int third=(inner-2*gap)/3;place(33,pad,254,third,34);place(34,pad+third+gap,254,third,34);place(35,pad+2*(third+gap),254,inner-2*(third+gap),34);
    place(30,pad,307,inner-64,24);place(31,width-pad-60,307,60,24);place(32,pad,335,inner,30);
    place(13,pad,383,inner,36);place(14,pad,433,inner,38);
    place(22,pad,(std::max)(492,int(r.bottom)-44),inner,24);
    InvalidateRect(h,nullptr,TRUE);
}
static LRESULT CALLBACK wndProc(HWND h,UINT m,WPARAM w,LPARAM l) {
    switch(m) {
    case WM_CREATE: {
        mainWindow=h;themeWindow(h);
        font=CreateFontW(-15,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        boldFont=CreateFontW(-24,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        smallFont=CreateFontW(-12,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        valueFont=CreateFontW(-22,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        stripFont=CreateFontW(-17,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        auto title=control(L"STATIC",L"Session Stats",0,24,22,440,36,20);SendMessageW(title,WM_SETFONT,(WPARAM)boldFont,TRUE);
        control(L"STATIC",L"Display mode",0,24,82,440,24,21);
        modeBox=control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_VSCROLL|WS_TABSTOP,24,112,440,170,10);
        SendMessageW(modeBox,CB_SETITEMHEIGHT,WPARAM(-1),32);
        SendMessageW(modeBox,CB_ADDSTRING,0,(LPARAM)L"Inside conversation");SendMessageW(modeBox,CB_ADDSTRING,0,(LPARAM)L"Floating window");SendMessageW(modeBox,CB_SETCURSEL,prefs.value("mode",0)==1?1:0,0);
        control(L"BUTTON",L"Start",BS_OWNERDRAW|WS_TABSTOP,24,164,214,40,11);
        control(L"BUTTON",L"Stop",BS_OWNERDRAW|WS_TABSTOP,250,164,214,40,12);
        control(L"STATIC",L"Floating window",0,24,224,440,24,36);
        control(L"BUTTON",L"Mini",BS_OWNERDRAW|WS_TABSTOP,24,254,140,34,33);
        control(L"BUTTON",L"Compact",BS_OWNERDRAW|WS_TABSTOP,174,254,140,34,34);
        control(L"BUTTON",L"Details",BS_OWNERDRAW|WS_TABSTOP,324,254,140,34,35);
        control(L"STATIC",L"Opacity",0,24,307,370,24,30);
        opacityValue=control(L"STATIC",L"",SS_RIGHT,404,307,60,24,31);
        opacitySlider=control(TRACKBAR_CLASSW,L"",TBS_HORZ|TBS_NOTICKS|WS_TABSTOP,24,335,440,30,32);
        SendMessageW(opacitySlider,TBM_SETRANGE,TRUE,MAKELPARAM(35,100));SendMessageW(opacitySlider,TBM_SETPOS,TRUE,std::clamp(prefs.value("floatOpacity",94),35,100));applyOpacity();
        autoBox=control(L"BUTTON",L"Auto-start with ChatGPT",BS_OWNERDRAW|WS_TABSTOP,24,236,440,36,13);
        autoEnabled=prefs.value("autoStart",false);
        control(L"BUTTON",L"Save startup setting",BS_OWNERDRAW|WS_TABSTOP,24,286,440,38,14);
        stateLabel=control(L"STATIC",L"Ready",0,24,348,440,32,22);
        selected=prefs.value("threadId",std::string());SetTimer(h,1,1000,nullptr);return 0;
    }
    case WM_SIZE:if(w!=SIZE_MINIMIZED)layoutDashboard(h);return 0;
    case WM_GETMINMAXINFO:((MINMAXINFO*)l)->ptMinTrackSize={420,570};return 0;
    case WM_HSCROLL:if((HWND)l==opacitySlider){prefs["floatOpacity"]=int(SendMessageW(opacitySlider,TBM_GETPOS,0,0));applyOpacity();if(LOWORD(w)!=TB_THUMBTRACK)savePrefs();}return 0;
    case WM_NOTIFY:if(((NMHDR*)l)->hwndFrom==opacitySlider&&((NMHDR*)l)->code==NM_CUSTOMDRAW){
        auto draw=(NMCUSTOMDRAW*)l;
        if(draw->dwDrawStage==CDDS_PREPAINT){FillRect(draw->hdc,&draw->rc,floatBrush);return CDRF_NOTIFYITEMDRAW;}
        if(draw->dwDrawStage==CDDS_ITEMPREPAINT){
            if(draw->dwItemSpec==TBCD_CHANNEL){RECT r=draw->rc;int y=(r.top+r.bottom)/2;r.top=y-3;r.bottom=y+3;floating_ui::roundRect(draw->hdc,r,RGB(47,55,72),RGB(47,55,72),6);r.right=r.left+(r.right-r.left)*int(SendMessageW(opacitySlider,TBM_GETPOS,0,0)-35)/65;if(r.right>r.left)floating_ui::roundRect(draw->hdc,r,accent,accent,6);return CDRF_SKIPDEFAULT;}
            if(draw->dwItemSpec==TBCD_THUMB){floating_ui::roundRect(draw->hdc,draw->rc,foreground,accent,10);return CDRF_SKIPDEFAULT;}
        }
    }break;
    case WM_MEASUREITEM:((MEASUREITEMSTRUCT*)l)->itemHeight=32;return TRUE;
    case WM_DRAWITEM:return drawControl((DRAWITEMSTRUCT*)l);
    case WM_TIMER:tick();return 0;
    case WM_CTLCOLORLISTBOX:SetTextColor((HDC)w,foreground);SetBkColor((HDC)w,background);return (LRESULT)floatBrush;
    case WM_CTLCOLORSTATIC:SetTextColor((HDC)w,GetDlgCtrlID((HWND)l)==20||GetDlgCtrlID((HWND)l)==31?foreground:muted);SetBkColor((HDC)w,background);return (LRESULT)floatBrush;
    case WM_APP+1:
        capturePreview(h,previewPath);
        if(floatWindow){
            latestStats={{"totalTokens",332251},{"lastTurnTokens",284550},{"averageOutputTps",17.6},{"cacheHitPercent",91.9},{"compactionCount",0},{"model","gpt-6-astra"},{"contextUsed",173000},{"contextWindow",258000}};
            for(auto size:std::vector<std::pair<int,int>>{{220,40},{460,48},{550,48},{620,48},{460,150},{460,288}}){SetWindowPos(floatWindow,nullptr,0,0,size.first,size.second,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);capturePreview(floatWindow,previewPath.parent_path()/(previewPath.stem().wstring()+L"-"+std::to_wstring(size.first)+L"x"+std::to_wstring(size.second)+L".bmp"));}
        }
        PostMessageW(h,WM_APP+3,0,0);return 0;
    case WM_APP+2:openFloating();return 0;
    case WM_APP+3:shuttingDown=true;DestroyWindow(h);return 0;
    case WM_APP+4:showDashboard();return 0;
    case WM_APP+5:showDashboard();startEmbedded();return 0;
    case WM_COMMAND: {
        int id=LOWORD(w);
        if(id==11) {
            int mode=int(SendMessageW(modeBox,CB_GETCURSEL,0,0));prefs["mode"]=mode;savePrefs();
            if(mode==0)startEmbedded();else openFloating();
        } else if(id==12) {waiting=false;if(floatWindow)DestroyWindow(floatWindow);runScript(L"Stop-EmbeddedStats.ps1",L"-Quiet");}
        else if(id==13){autoEnabled=!autoEnabled;InvalidateRect(autoBox,nullptr,TRUE);}
        else if(id==14) {prefs["autoStart"]=autoEnabled;savePrefs();runScript(L"Install-Shortcuts.ps1",L"-ConfigureCodexEntry -AutoStart "+std::to_wstring(autoEnabled?1:0));}
        else if(id==33)setFloatingSize(460,48);
        else if(id==34)setFloatingSize(460,150);
        else if(id==35)setFloatingSize(460,288);
        return 0;
    }
    case WM_CLOSE:waiting=false;if(floatWindow){ShowWindow(h,SW_HIDE);return 0;}DestroyWindow(h);return 0;
    case WM_DESTROY:shuttingDown=true;waiting=false;if(floatWindow)DestroyWindow(floatWindow);KillTimer(h,1);PostQuitMessage(0);return 0;
    }return DefWindowProcW(h,m,w,l);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show) {
    int argc=0;LPWSTR* args=CommandLineToArgvW(GetCommandLineW(),&argc);
    bool startFloating=argc==2 && std::wstring(args[1])==L"--floating";
    bool embeddedRequested=argc==2 && std::wstring(args[1])==L"--embedded";
    bool quitRequested=argc==2 && std::wstring(args[1])==L"--quit";
    if(argc==3 && std::wstring(args[1])==L"--smoke-test")smokePath=args[2];
    if(argc==3 && std::wstring(args[1])==L"--preview")previewPath=args[2];
    LocalFree(args);
    const bool testMode=!smokePath.empty()||!previewPath.empty();
    std::wstring mutexName=L"Local\\CodexSessionStatsPanel";
    if(testMode)mutexName+=L"Test"+std::to_wstring(GetCurrentProcessId());
    HANDLE mutex=CreateMutexW(nullptr,FALSE,mutexName.c_str());
    if(GetLastError()==ERROR_ALREADY_EXISTS){HWND old=FindWindowW(L"CodexSessionStatsPanel",nullptr);if(old)PostMessageW(old,quitRequested?WM_APP+3:startFloating?WM_APP+2:embeddedRequested?WM_APP+5:WM_APP+4,0,0);CloseHandle(mutex);return 0;}
    if(quitRequested){CloseHandle(mutex);return 0;}
    wchar_t path[32768];GetModuleFileNameW(nullptr,path,32768);root=fs::path(path).parent_path();
    wchar_t* value=_wgetenv(L"CODEX_HOME");if(value && *value)home=value;else {value=_wgetenv(L"USERPROFILE");home=fs::path(value?value:L".")/L".codex";}
    value=_wgetenv(L"LOCALAPPDATA");stateRoot=fs::path(value?value:L".")/L"CodexSessionMonitor";fs::create_directories(stateRoot);resultPath=stateRoot/L"panel-result.txt";
    try {prefs=json::parse(textFile(stateRoot/L"preferences.json"));if(!prefs.is_object())prefs=json::object();}catch(...){prefs=json::object();}
    reader=std::make_unique<SessionStatsReader>(home);floatBrush=CreateSolidBrush(background);
    INITCOMMONCONTROLSEX common{};common.dwSize=sizeof(common);common.dwICC=ICC_BAR_CLASSES|ICC_WIN95_CLASSES;InitCommonControlsEx(&common);
    WNDCLASSW cls{};cls.hInstance=instance;cls.lpfnWndProc=wndProc;cls.lpszClassName=L"CodexSessionStatsPanel";cls.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1));cls.hCursor=LoadCursor(nullptr,IDC_ARROW);cls.hbrBackground=floatBrush;RegisterClassW(&cls);
    cls.lpfnWndProc=floatProc;cls.lpszClassName=L"CodexSessionStatsFloating";RegisterClassW(&cls);
    HWND window=CreateWindowExW(0,L"CodexSessionStatsPanel",L"Session Stats - Dashboard",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,510,590,nullptr,nullptr,instance,nullptr);
    if(startFloating||testMode){SendMessageW(modeBox,CB_SETCURSEL,1,0);openFloating();}
    if(embeddedRequested){SendMessageW(modeBox,CB_SETCURSEL,0,0);startEmbedded();}
    if(!smokePath.empty()&&selected.empty()){std::ofstream(smokePath)<<"{\"error\":\"no-recent-conversations\"}";PostMessageW(window,WM_APP+3,0,0);}
    ShowWindow(window,smokePath.empty()?show:SW_HIDE);
    if(!previewPath.empty()){UpdateWindow(window);UpdateWindow(floatWindow);PostMessageW(window,WM_APP+1,0,0);}
    MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){if(!(floatWindow&&IsDialogMessageW(floatWindow,&msg))&&!IsDialogMessageW(window,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
    if(pendingRead.valid())pendingRead.wait();if(launchProcess)CloseHandle(launchProcess);DeleteObject(font);DeleteObject(boldFont);DeleteObject(smallFont);DeleteObject(valueFont);DeleteObject(stripFont);DeleteObject(floatBrush);CloseHandle(mutex);return 0;
}
