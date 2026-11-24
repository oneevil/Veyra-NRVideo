// SPDX-License-Identifier: GPL-3.0-only
#include "veyra/engine/SubtitleFinder.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace veyra::engine::screentext {
namespace {

float tallOf(const FoundBox& b){return b.bottom-b.top;}
float wideOf(const FoundBox& b){return b.right-b.left;}
float middleY(const FoundBox& b){return (b.top+b.bottom)/2;}

float median(std::vector<float> v){
    if(v.empty())return 0;
    std::sort(v.begin(),v.end());
    const size_t n=v.size();
    return n%2?v[n/2]:(v[n/2-1]+v[n/2])/2;
}

// The text finder cuts a line of large letters into words: words side by side at one height and
// of one size are one line again.
std::vector<FoundBox> joinWords(std::vector<FoundBox> boxes){
    std::sort(boxes.begin(),boxes.end(),[](const FoundBox& a,const FoundBox& b){
        return a.left!=b.left?a.left<b.left:a.top!=b.top?a.top<b.top:a.right!=b.right?a.right<b.right:a.bottom<b.bottom;});
    std::vector<FoundBox> lines;
    for(const auto& b:boxes){
        bool joined=false;
        for(auto& line:lines){
            const float tall=std::min(tallOf(line),tallOf(b));
            const float shared=std::min(line.bottom,b.bottom)-std::max(line.top,b.top);
            const float ratio=tallOf(line)>0?tallOf(b)/tallOf(line):0;
            if(shared>0.6f*tall&&b.left-line.right<1.5f*tall&&ratio>=0.7f&&ratio<=1.43f){
                line.top=std::min(line.top,b.top);line.right=std::max(line.right,b.right);line.bottom=std::max(line.bottom,b.bottom);
                joined=true;break;
            }
        }
        if(!joined)lines.push_back(b);
    }
    return lines;
}

bool samePlace(const FoundBox& a,const FoundBox& b,float slack){
    return std::abs(a.left-b.left)<=slack&&std::abs(a.top-b.top)<=slack&&std::abs(a.right-b.right)<=slack&&std::abs(a.bottom-b.bottom)<=slack;
}

struct Seen { int look; FoundBox box; };

} // namespace

std::optional<FoundArea> locateSubtitles(const std::vector<std::vector<FoundBox>>& raw,int width,int height){
    const int total=int(raw.size());
    if(total<4||width<=0||height<=0)return std::nullopt;
    std::vector<std::vector<FoundBox>> looks;
    for(const auto& boxes:raw)looks.push_back(joinWords(boxes));
    const float W=float(width),H=float(height);
    const float slack=std::max(6.0f,W/200);

    // The game's own text: in the same place in four looks out of five.
    std::vector<FoundBox> fixed;
    for(const auto& boxes:looks)for(const auto& b:boxes){
        int in=0;
        for(const auto& other:looks)
            if(std::any_of(other.begin(),other.end(),[&](const FoundBox& o){return samePlace(b,o,slack);}))++in;
        if(in>=0.8*total)fixed.push_back(b);
    }
    // Text shaped like a line of words, not standing still.
    std::vector<Seen> lines;
    for(int look=0;look<total;++look)for(const auto& b:looks[size_t(look)]){
        if(std::any_of(fixed.begin(),fixed.end(),[&](const FoundBox& f){return samePlace(b,f,slack);}))continue;
        const float tall=tallOf(b),wide=wideOf(b);
        // (48 px: a line of a film in a small window is still a line, whatever the screen's size)
        if(tall<0.012f*H||tall>std::max(0.09f*H,48.0f)||wide<0.05f*W||wide<3.0f*tall)continue;
        lines.push_back({look,b});
    }
    if(lines.empty())return std::nullopt;

    // Rows: lines at one height in letters of one size, whichever look they were in.
    std::stable_sort(lines.begin(),lines.end(),[](const Seen& a,const Seen& b){return middleY(a.box)<middleY(b.box);});
    std::vector<std::vector<Seen>> rows;
    for(const auto& s:lines){
        bool placed=false;
        for(auto& row:rows){
            const auto& first=row.front().box;
            const float ratio=tallOf(s.box)/tallOf(first);
            if(std::abs(middleY(first)-middleY(s.box))<=0.3f*tallOf(first)&&ratio>=0.75f&&ratio<=1.33f){row.push_back(s);placed=true;break;}
        }
        if(!placed)rows.push_back({s});
    }
    auto looksIn=[](const std::vector<Seen>& row){std::set<int> l;for(const auto& s:row)l.insert(s.look);return int(l.size());};
    auto middleX=[](const FoundBox& b){return (b.left+b.right)/2;};
    // The middle most of a row's lines share, and those lines: something else may stand at the
    // same height (a hint in a corner), and a player's panels move a film off the picture's middle.
    const float near=std::max(6.0f,0.012f*W);
    auto onAxis=[&](const std::vector<Seen>& row,std::vector<Seen>& kept){
        float best=middleX(row.front().box);int bestLooks=-1;
        for(const auto& c:row){
            std::set<int> l;
            for(const auto& o:row)if(std::abs(middleX(o.box)-middleX(c.box))<=near)l.insert(o.look);
            if(int(l.size())>bestLooks){bestLooks=int(l.size());best=middleX(c.box);}
        }
        kept.clear();
        std::vector<float> m;
        for(const auto& o:row)if(std::abs(middleX(o.box)-best)<=near){kept.push_back(o);m.push_back(middleX(o.box));}
        return median(m);
    };
    struct Score { int value=0; bool middle=false; };
    // How likely a row is the subtitles: seen in several looks, changing length, and centred - on
    // the picture or on a middle of its own - or from one left edge low on the screen (higher up
    // such lines are a game's hints); low and centred is the likeliest.
    auto score=[&](const std::vector<Seen>& row)->Score{
        int seen=looksIn(row);
        std::set<long> lengths;
        std::vector<float> offCentre,lefts,middles;
        for(const auto& s:row){
            lengths.insert(std::lround(wideOf(s.box)/(0.04f*W)));
            offCentre.push_back(std::abs(middleX(s.box)-W/2));
            lefts.push_back(s.box.left);
            middles.push_back(middleY(s.box));
        }
        const bool centred=median(offCentre)<=0.06f*W;
        const bool low=median(middles)>0.55f*H;
        const auto [minLeft,maxLeft]=std::minmax_element(lefts.begin(),lefts.end());
        const bool oneEdge=low&&*maxLeft-*minLeft<=0.02f*W&&*minLeft<0.5f*W;
        bool axis=false;
        if(low&&!centred&&!oneEdge){
            std::vector<Seen> kept;
            onAxis(row,kept);
            std::set<long> keptLengths;
            float narrow=1e9f,wide=0;
            for(const auto& s:kept){
                keptLengths.insert(std::lround(wideOf(s.box)/(0.04f*W)));
                narrow=std::min(narrow,wideOf(s.box));wide=std::max(wide,wideOf(s.box));
            }
            axis=looksIn(kept)>=3&&keptLengths.size()>=2&&wide-narrow>=0.06f*W;
            if(axis)seen=looksIn(kept);
        }
        if(seen<3||lengths.size()<2||!(centred||oneEdge||axis))return {};
        return {seen*(centred?3:2)*(low?3:2),centred||axis};
    };
    std::vector<Score> scores;
    size_t best=0;
    for(size_t i=0;i<rows.size();++i){
        scores.push_back(score(rows[i]));
        if(scores[i].value>scores[best].value)best=i;
    }
    if(!scores[best].value)return std::nullopt;
    const bool inMiddle=scores[best].middle;
    std::vector<Seen> main=rows[best];
    float axis=0;
    if(inMiddle){std::vector<Seen> kept;axis=onAxis(main,kept);main=kept;}
    std::vector<float> talls,centres,mids;
    for(const auto& s:main){talls.push_back(tallOf(s.box));centres.push_back(middleY(s.box));mids.push_back(middleX(s.box));}
    const float tall=median(talls),centre=median(centres);
    auto inMain=[&](const Seen& s){
        return std::any_of(main.begin(),main.end(),[&](const Seen& m){return m.look==s.look&&samePlace(m.box,s.box,0.0f);});
    };
    // The other lines of the same subtitles: rows within two lines of it (a second line is rarer,
    // so two looks are enough for it) - around the same middle when the lines are centred.
    std::vector<FoundBox> band;
    for(const auto& s:main)band.push_back(s.box);
    for(size_t i=0;i<rows.size();++i){
        std::vector<Seen> row;
        if(inMiddle){
            for(const auto& s:rows[i])if(std::abs(middleX(s.box)-axis)<=0.03f*W&&!inMain(s))row.push_back(s);
            if(row.empty())continue;
        }else{
            if(i==best)continue;
            row=rows[i];
        }
        const auto& first=row.front().box;
        const float ratio=tallOf(first)/tall;
        const bool close=std::abs(middleY(first)-centre)<=2.6f*tall&&ratio>=0.75f&&ratio<=1.33f;
        if(close&&(scores[i].value||looksIn(row)>=2))for(const auto& s:row)band.push_back(s.box);
    }
    float left=band.front().left,right=band.front().right,top=band.front().top,bottom=band.front().bottom;
    for(const auto& b:band){left=std::min(left,b.left);right=std::max(right,b.right);top=std::min(top,b.top);bottom=std::max(bottom,b.bottom);}
    const float middle=median(mids);
    if(inMiddle){
        // Centred lines: a longer one reaches further both ways - how far is told by the letters
        // (the longest lines are 20-27 times as wide as tall) as much as by the longest line seen,
        // which twelve seconds may not have shown; not by the screen, which a film in a window is
        // only a part of.
        const float half=std::min(0.47f*W,std::max(13.0f*tall,1.3f*std::max(middle-left,right-middle)));
        left=middle-half;right=middle+half;
    }else{
        left-=0.03f*W;right+=0.25f*W; // lines from the left edge grow to the right
    }
    top-=1.5f*tall;bottom+=1.5f*tall; // room for one more line either way
    FoundArea out;
    out.left=std::max(0,int(left));out.top=std::max(0,int(top));
    const int r=std::min(width,int(right)),b=std::min(height,int(bottom));
    out.width=r-out.left;out.height=b-out.top;
    out.looks=looksIn(main);out.of=total;
    if(out.width<=0||out.height<=0)return std::nullopt;
    return out;
}

} // namespace veyra::engine::screentext
