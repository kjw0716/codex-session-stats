#define NOMINMAX
#include "../native/floating-ui.hpp"
#include <cassert>
#include <iostream>
int main(){
    using namespace floating_ui;
    json stats={{"totalTokens",897800},{"averageOutputTps",20.5},{"cacheHitPercent",92.2},{"lastTurnTokens",120000},{"compactionCount",4},{"contextUsed",173000},{"contextWindow",258000}};
    assert(value(stats,6)==L"173/258K");assert(value(stats,6,false)==L"173000/258000");
    stats["totalTokens"]=900000000;assert(contextLevel(stats)==0);
    stats["contextWindow"]=1000;
    for(auto p:std::vector<std::pair<int,int>>{{699,0},{700,1},{849,1},{850,2},{1200,2}}){stats["contextUsed"]=p.first;assert(contextLevel(stats)==p.second);}
    stats["contextWindow"]=0;assert(contextLevel(stats)==-1);assert(value(stats,6)==L"--/--");
    stats["contextWindow"]=nullptr;assert(contextLevel(stats)==-1);
    stats["contextWindow"]=258000;stats["contextUsed"]=173000;
    HDC dc=CreateCompatibleDC(nullptr);
    HFONT small=CreateFontW(-12,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,0,0,L"Segoe UI");
    HFONT large=CreateFontW(-17,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,0,0,L"Segoe UI");
    auto previous=SelectObject(dc,small);
    for(long long tokens:{0LL,897800LL,900000000LL,999999999999999LL}){
        stats["totalTokens"]=tokens;
        for(int width=220;width<=1100;++width){
            auto layout=measureStrip(dc,width,48,stats,small,large);int last=-1;
            for(const auto& cell:layout.cells){
                assert(cell.rect.right<=width-42);assert(cell.rect.left>=14);
                if(last>=0){assert(cell.rect.left-last==stripGap(dc,large));assert(cell.rect.left-last<=textWidth(dc,large,L"   "));}
                assert(cell.rect.right-cell.rect.left==cellWidth(dc,stats,cell.metric,cell.label,small,large));last=cell.rect.right;
            }
            // A skipped metric must genuinely require more space than remains.
            const int order[]={0,6,2,3,1,4};const wchar_t* labels[]={L"Tokens",L"",L"",L"Cache",L"Last",L"Compacts"};
            int cursor=14;
            for(int i=0;i<6;++i){bool present=false;for(auto& cell:layout.cells)if(cell.metric==order[i]){present=true;cursor=cell.rect.right+stripGap(dc,large);break;}
                if(!present)assert(cursor+cellWidth(dc,stats,order[i],labels[i],small,large)>width-42);
            }
        }
    }
    stats["totalTokens"]=897800;
    auto packed=measureStrip(dc,550,48,stats,small,large);assert(packed.cells.size()>=4);assert(packed.cells[1].metric==6);
    int exact=14+cellWidth(dc,stats,0,L"Tokens",small,large)+stripGap(dc,large)+cellWidth(dc,stats,6,L"",small,large)+42;
    assert(measureStrip(dc,exact,48,stats,small,large).cells[1].metric==6);
    for(auto& c:measureStrip(dc,exact-1,48,stats,small,large).cells)assert(c.metric!=6);
    SelectObject(dc,previous);DeleteObject(small);DeleteObject(large);DeleteDC(dc);
    std::cout<<"PASS actual-font packing, tight gaps, fit boundary, context formatting and warning thresholds\n";
}
