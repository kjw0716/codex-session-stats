#pragma once
#include <windows.h>
#include <array>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <cmath>
#include "vendor/json.hpp"

namespace floating_ui {
using json=nlohmann::json;
constexpr COLORREF bg=RGB(20,23,31), text=RGB(237,241,250), secondary=RGB(142,155,177), violet=RGB(160,146,255), mint=RGB(101,224,189);
struct Cell { int metric; RECT rect; std::wstring label; };
struct Layout { bool strip=false, picker=false; RECT menu{}; std::vector<Cell> cells; };
inline Layout measure(int w,int h) {
    Layout out;out.strip=h<78;out.picker=h>=118;
    out.menu={w-35,8,w-7,out.strip?h-8:36};
    if(out.strip){
        // Geometry-only fallback; paint packs the actual measured text via measureStrip.
        out.cells={{0,{14,4,w-42,h-4},L"Tokens"}};
        return out;
    }
    int top=out.picker?50:10,bottom=h-10;
    // When there is no picker the menu occupies its own small gutter.
    int right=out.picker?w-10:w-42;
    int count=h>=330?7:h>=252?6:h>=144?4:2,cols=w>=570&&count>=6?3:2,rows=(count+cols-1)/cols;
    int gap=8,cellW=(right-10-gap*(cols-1))/cols,cellH=(bottom-top-gap*(rows-1))/rows;
    const int order[]={0,6,2,3,1,4,5};
    const wchar_t* full[]={L"Total session tokens",L"Last turn",L"Output speed",L"Cache hit rate",L"Compactions",L"Model",L"Context usage"};
    const wchar_t* shortNames[]={L"Tokens",L"Last",L"Speed",L"Cache",L"Compacts",L"Model",L"Context"};
    for(int i=0;i<count;++i){int col=i%cols,row=i/cols,index=order[i];RECT r={10+col*(cellW+gap),top+row*(cellH+gap),10+col*(cellW+gap)+cellW,top+row*(cellH+gap)+cellH};if(i==count-1&&count%cols==1)r.right=right;out.cells.push_back({index,r,cellW>=205&&cellH>=56?full[index]:shortNames[index]});}
    return out;
}
inline std::wstring fromUtf8(const std::string& s){int n=MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),nullptr,0);std::wstring out(n,0);if(n)MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),out.data(),n);return out;}
inline int contextLevel(const json& stats){
    if(!stats.contains("contextUsed")||!stats["contextUsed"].is_number()||!stats.contains("contextWindow")||!stats["contextWindow"].is_number())return -1;
    double used=stats["contextUsed"].get<double>(),capacity=stats["contextWindow"].get<double>();
    if(!std::isfinite(used)||!std::isfinite(capacity)||used<0||capacity<=0)return -1;
    double percent=used/capacity*100;return percent>=85?2:percent>=70?1:0;
}
inline COLORREF contextColor(const json& stats){int level=contextLevel(stats);return level<0?secondary:level==2?RGB(255,107,124):level==1?RGB(255,195,82):RGB(101,224,153);}
inline std::wstring value(const json& stats,int index,bool compact=true){
    if(index==6){
        if(contextLevel(stats)<0)return L"--/--";
        double used=stats["contextUsed"].get<double>(),capacity=stats["contextWindow"].get<double>(),scale=compact&&capacity>=1000?1000:1;
        auto number=[&](double n){std::wostringstream text;text<<std::fixed<<std::setprecision(scale>1&&n>0&&n<10000?1:0)<<n/scale;return text.str();};
        return number(used)+L"/"+number(capacity)+(scale>1?L"K":L"");
    }
    const char* keys[]={"totalTokens","lastTurnTokens","averageOutputTps","cacheHitPercent","compactionCount","model"};
    if(!stats.contains(keys[index])||stats[keys[index]].is_null())return L"--";
    if(index==5)return stats[keys[index]].is_string()?fromUtf8(stats[keys[index]].get<std::string>()):L"--";
    if(!stats[keys[index]].is_number())return L"--";
    double number=stats[keys[index]].get<double>();std::wostringstream result;
    if(index==2||index==3){result<<std::fixed<<std::setprecision(1)<<number<<(index==2?L" tok/s":L"%");return result.str();}
    const wchar_t* unit=L"";
    if(compact&&number>=1e9){number/=1e9;unit=L"B";}else if(compact&&number>=1e6){number/=1e6;unit=L"M";}else if(compact&&number>=1e3){number/=1e3;unit=L"K";}
    result<<std::fixed<<std::setprecision(*unit?(number<10?2:1):0)<<number<<unit;return result.str();
}
inline int textWidth(HDC dc,HFONT font,const std::wstring& text){SelectObject(dc,font);SIZE size{};GetTextExtentPoint32W(dc,text.c_str(),int(text.size()),&size);return size.cx;}
inline int stripGap(HDC dc,HFONT font){return (std::max)(2,textWidth(dc,font,L"  "));}
inline int labelGap(HDC dc,HFONT font){return (std::max)(1,textWidth(dc,font,L" "));}
inline int cellWidth(HDC dc,const json& stats,int metric,const std::wstring& title,HFONT small,HFONT large){
    return textWidth(dc,large,value(stats,metric))+(title.empty()?0:textWidth(dc,small,title)+labelGap(dc,small))+(metric==6?12:0);
}
inline Layout measureStrip(HDC dc,int w,int h,const json& stats,HFONT small,HFONT large){
    Layout out=measure(w,h);out.cells.clear();int cursor=14,end=w-42,gap=stripGap(dc,large);
    // Context precedes the optional metrics so its warning remains visible on narrow widgets.
    const int order[]={0,6,2,3,1,4};const wchar_t* titles[]={L"Tokens",L"",L"",L"Cache",L"Last",L"Compacts"};
    for(int i=0;i<6;++i){int length=cellWidth(dc,stats,order[i],titles[i],small,large);
        if(cursor+length>end){if(i==0){out.cells.push_back({0,{cursor,4,end,h-4},L"Tokens"});return out;}continue;}
        out.cells.push_back({order[i],{cursor,4,cursor+length,h-4},titles[i]});cursor+=length+gap;
    }
    return out;
}
inline void contextDot(HDC dc,RECT& r,const json& stats){
    int y=(r.top+r.bottom)/2;HBRUSH brush=CreateSolidBrush(contextColor(stats));auto oldB=SelectObject(dc,brush);auto oldP=SelectObject(dc,GetStockObject(NULL_PEN));Ellipse(dc,r.left,y-4,r.left+8,y+4);SelectObject(dc,oldB);SelectObject(dc,oldP);DeleteObject(brush);r.left+=12;
}
inline void roundRect(HDC dc,RECT r,COLORREF fill,COLORREF border,int radius=12){
    HBRUSH b=CreateSolidBrush(fill);HPEN p=CreatePen(PS_SOLID,1,border);auto oldB=SelectObject(dc,b);auto oldP=SelectObject(dc,p);RoundRect(dc,r.left,r.top,r.right,r.bottom,radius,radius);SelectObject(dc,oldB);SelectObject(dc,oldP);DeleteObject(b);DeleteObject(p);
}
inline void label(HDC dc,const std::wstring& s,RECT r,HFONT font,COLORREF color,UINT flags=DT_LEFT|DT_VCENTER){SelectObject(dc,font);SetTextColor(dc,color);SetBkMode(dc,TRANSPARENT);DrawTextW(dc,s.c_str(),int(s.size()),&r,flags|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);}
inline void paint(HDC dc,int w,int h,const json& stats,HFONT regular,HFONT small,HFONT valueFont,HFONT stripFont){
    RECT all={0,0,w,h};HBRUSH brush=CreateSolidBrush(bg);FillRect(dc,&all,brush);DeleteObject(brush);
    roundRect(dc,{0,0,w,h},bg,RGB(64,72,92),14);
    auto layout=h<78?measureStrip(dc,w,h,stats,small,stripFont):measure(w,h);
    // A narrow colored edge gives the widget an identity without adding chrome.
    roundRect(dc,{3,10,6,h-10},violet,violet,3);
    for(const auto& cell:layout.cells){
        COLORREF color=cell.metric==2?mint:cell.metric==3?violet:text;
        if(layout.strip){
            RECT r=cell.rect;if(cell.metric==6)contextDot(dc,r,stats);
            if(!cell.label.empty()){
                SelectObject(dc,small);SIZE size{};GetTextExtentPoint32W(dc,cell.label.c_str(),int(cell.label.size()),&size);
                RECT title=r;title.right=(std::min)(r.right,r.left+size.cx);label(dc,cell.label,title,small,secondary);r.left+=size.cx+labelGap(dc,small);
            }
            label(dc,value(stats,cell.metric),r,stripFont,color);
        }else{
            roundRect(dc,cell.rect,cell.metric==0?RGB(37,34,57):cell.metric==2?RGB(25,45,45):RGB(28,33,43),cell.metric==0?RGB(65,56,91):RGB(43,51,66),12);
            RECT r=cell.rect;r.left+=12;r.right-=10;
            if(r.bottom-r.top>=56){RECT title=r;title.top+=8;title.bottom=title.top+18;label(dc,cell.label,title,small,secondary);r.top+=28;r.bottom-=5;if(cell.metric==6)contextDot(dc,r,stats);label(dc,value(stats,cell.metric),r,cell.metric==5?regular:valueFont,color);}
            else {r.top+=2;r.bottom-=2;RECT title=r;title.right=r.left+(r.right-r.left)/3;label(dc,cell.label,title,small,secondary);r.left=title.right+6;if(cell.metric==6){r.left=(std::max)(r.left,r.right-textWidth(dc,regular,value(stats,6))-12);contextDot(dc,r,stats);}label(dc,value(stats,cell.metric),r,regular,color,DT_RIGHT|DT_VCENTER);}
        }
    }
    RECT menu=layout.menu;
    if(!layout.strip)roundRect(dc,menu,RGB(32,38,49),RGB(44,53,70),9);
    int centerX=(menu.left+menu.right)/2,centerY=(menu.top+menu.bottom)/2;
    brush=CreateSolidBrush(secondary);for(int dx:{-6,0,6}){RECT dot={centerX+dx-1,centerY-1,centerX+dx+2,centerY+2};FillRect(dc,&dot,brush);}DeleteObject(brush);
    // A subtle resize grip remains available even in the one-line layout.
    HPEN pen=CreatePen(PS_SOLID,1,RGB(87,95,116));auto old=SelectObject(dc,pen);for(int n:{4,7}){MoveToEx(dc,w-n,h-3,nullptr);LineTo(dc,w-3,h-n);}SelectObject(dc,old);DeleteObject(pen);
}
}
