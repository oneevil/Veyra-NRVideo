// Telling the subtitle from text in the scene (SubtitleStyle.h).
#include "SubtitleStyle.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace veyra::engine::screentext {
using namespace subtext;
using Rgb=SubtitleStyle::Rgb;

namespace {
constexpr int kTolerance=45;      // per channel, how far a pixel may be from a colour and still be of it
constexpr float kSupported=0.5f;  // share of a word's width that must be letters of the colour
constexpr float kPatches=0.6f;    // letter-high patches of the colour per letter read
constexpr float kSaturated=0.12f; // from here a colour is a colour, not a shade of white (pale names: 0.14-0.16)
constexpr float kLevel=4.0f;      // degrees a piece may lean
// Latin letters a short Russian word may come back in ("OH?" for "Он?", "Het!" for "Нет!").
constexpr std::wstring_view kRussianLooking=L"aceopxyABCEHKMOPTXtkmunhbr";

struct Box {
    int l=0,t=0,r=0,b=0;
    int w()const{return r-l;}
    int h()const{return b-t;}
};
Box boxOf(const Piece& p){const auto b=p.box();return {b.left,b.top,b.right,b.bottom};}

// Where letters of a colour stand in a box: per column whether one stands there, the middles of
// the letter-high patches (from the box's left), and how tall those letters are.
struct Letters {
    std::vector<uint8_t> cols;
    std::vector<float> mids;
    float height=0;
};

float percentile(std::vector<float> v,double q){
    if(v.empty())return 0;
    std::sort(v.begin(),v.end());
    const double pos=q/100.0*double(v.size()-1);
    const size_t lo=size_t(pos),hi=std::min(lo+1,v.size()-1);
    return float(v[lo]+(v[hi]-v[lo])*(pos-double(lo)));
}
float median(std::vector<float> v){return percentile(std::move(v),50);}
Rgb medianColour(const std::vector<Rgb>& cs){
    Rgb o{};
    for(int ch=0;ch<3;++ch){std::vector<float> v;v.reserve(cs.size());for(const auto& c:cs)v.push_back(c[ch]);o[ch]=median(v);}
    return o;
}
float maxDiff(const Rgb& a,const Rgb& b){return std::max({std::abs(a[0]-b[0]),std::abs(a[1]-b[1]),std::abs(a[2]-b[2])});}
float saturation(const Rgb& c){
    const float mx=std::max({c[0],c[1],c[2]}),mn=std::min({c[0],c[1],c[2]});
    return (mx-mn)/std::max(mx,1.0f);
}
float hueDeg(float r,float g,float b){
    const float mx=std::max({r,g,b}),mn=std::min({r,g,b}),d=mx-mn;
    if(d<=0)return 0;
    float h=mx==r?(g-b)/d:mx==g?2.0f+(b-r)/d:4.0f+(r-g)/d;
    h*=60.0f;
    return h<0?h+360.0f:h;
}
float hueDeg(const Rgb& c){return hueDeg(c[0],c[1],c[2]);}
// Whether this is another colour than the lines' own (shades of white and grey are no colour).
bool coloured(const Rgb& c,const std::optional<Rgb>& body){
    if(saturation(c)<kSaturated)return false;
    if(!body||saturation(*body)<kSaturated)return true;
    const float d=std::fmod(std::abs(hueDeg(c)-hueDeg(*body)),360.0f);
    return std::min(d,360.0f-d)>30.0f;
}
size_t alnumCount(const std::wstring& s){return size_t(std::count_if(s.begin(),s.end(),isAlnum));}
bool endsInColon(const std::wstring& s){
    size_t e=s.size();while(e&&isSpace(s[e-1]))--e;
    return e&&(s[e-1]==L':'||s[e-1]==L'：');
}

// 1-D running minimum or maximum over [x-r, x+r], ignoring what lies outside (van Herk).
void runMinMax(const int* in,int n,int r,bool takeMin,int* out){
    const int id=takeMin?1<<20:-(1<<20),k=2*r+1,m=n+2*r;
    auto op=[&](int a,int b){return takeMin?std::min(a,b):std::max(a,b);};
    std::vector<int> pad(size_t(m),id),g(size_t(m),0),h(size_t(m),0);
    for(int i=0;i<n;++i)pad[size_t(i+r)]=in[i];
    for(int i=0;i<m;++i)g[size_t(i)]=i%k==0?pad[size_t(i)]:op(g[size_t(i-1)],pad[size_t(i)]);
    for(int i=m-1;i>=0;--i)h[size_t(i)]=(i%k==k-1||i==m-1)?pad[size_t(i)]:op(h[size_t(i+1)],pad[size_t(i)]);
    for(int x=0;x<n;++x)out[x]=op(h[size_t(x)],g[size_t(x+k-1)]);
}
// Erosion or dilation with an elliptic element of size k (k odd), as image libraries draw it.
std::vector<int> morph(const std::vector<int>& img,int w,int h,int k,bool erode){
    const int r=k/2;
    std::vector<int> half(size_t(k),0);
    for(int i=0;i<k;++i){const int dy=i-r;half[size_t(i)]=r?int(std::lround(r*std::sqrt(double(r*r-dy*dy)/double(r*r)))):0;}
    std::map<int,std::vector<int>> rowRun;
    for(int hw:half){
        if(rowRun.count(hw))continue;
        auto& v=rowRun[hw];v.resize(size_t(w)*h);
        for(int y=0;y<h;++y)runMinMax(&img[size_t(y)*w],w,hw,erode,&v[size_t(y)*w]);
    }
    std::vector<int> out(size_t(w)*h,erode?1<<20:-(1<<20));
    for(int i=0;i<k;++i){
        const int dy=i-r;const auto& v=rowRun[half[size_t(i)]];
        for(int y=std::max(0,-dy);y<h&&y+dy<h;++y){
            const int* src=&v[size_t(y+dy)*w];int* dst=&out[size_t(y)*w];
            for(int x=0;x<w;++x)dst[x]=erode?std::min(dst[x],src[x]):std::max(dst[x],src[x]);
        }
    }
    return out;
}
} // namespace

namespace detail {
// A frame's crop, with the "what lies behind" pictures cached (several questions share a box).
class StylePicture {
public:
    explicit StylePicture(const Crop& c):c_(c){}
    int W()const{return int(c_.w);}
    int H()const{return int(c_.h);}
    bool ok()const{return c_.rgba.size()>=size_t(c_.w)*c_.h*4;}
    const uint8_t* at(int x,int y)const{return &c_.rgba[(size_t(y)*c_.w+size_t(x))*4];}
    Box clip(Box b)const{
        b.l=std::clamp(b.l,0,W());b.r=std::clamp(b.r,0,W());b.t=std::clamp(b.t,0,H());b.b=std::clamp(b.b,0,H());
        return b;
    }
    // The box with a margin of a quarter of its height around it; dx, dy: the box within the cut.
    struct Cut { int x0=0,y0=0,x1=0,y1=0,dx=0,dy=0; int w()const{return x1-x0;} int h()const{return y1-y0;} };
    Cut cut(const Box& b)const{
        const int m=std::max(2,int(0.25*b.h()));
        Cut c;c.x0=std::max(0,b.l-m);c.y0=std::max(0,b.t-m);c.x1=std::min(W(),b.r+m);c.y1=std::min(H(),b.b+m);
        c.dx=b.l-c.x0;c.dy=b.t-c.y0;
        return c;
    }
    // The cut with everything thin wiped out: a median of k x k per channel, edges repeated.
    const std::vector<uint8_t>& behind(const Cut& c,int k){
        auto& out=behind_[{c.x0,c.y0,c.x1,c.y1,k}];
        if(!out.empty())return out;
        const int w=c.w(),h=c.h(),r=k/2,target=(k*k)/2;
        out.resize(size_t(w)*h*3);
        auto px=[&](int x,int y,int ch){return at(c.x0+std::clamp(x,0,w-1),c.y0+std::clamp(y,0,h-1))[ch];};
        for(int ch=0;ch<3;++ch)for(int y=0;y<h;++y){
            int hist[256]={};
            auto column=[&](int x,int d){for(int yy=y-r;yy<=y+r;++yy)hist[px(x,yy,ch)]+=d;};
            for(int x=-r;x<=r;++x)column(x,1);
            int med=0,below=0;
            for(int x=0;x<w;++x){
                if(x){
                    for(int yy=y-r;yy<=y+r;++yy){const int v=px(x-1-r,yy,ch);--hist[v];if(v<med)--below;}
                    for(int yy=y-r;yy<=y+r;++yy){const int v=px(x+r,yy,ch);++hist[v];if(v<med)++below;}
                }
                while(below>target){--med;below-=hist[med];}
                while(below+hist[med]<=target){below+=hist[med];++med;}
                out[(size_t(y)*w+x)*3+ch]=uint8_t(med);
            }
        }
        return out;
    }
private:
    const Crop& c_;
    std::map<std::array<int,5>,std::vector<uint8_t>> behind_;
};
} // namespace detail

namespace {
using Pic=detail::StylePicture;

// The colour of the thin strokes in the box that stand out lighter (or darker) than around them.
std::optional<Rgb> ownColour(Pic& pic,Box box,bool dark=false){
    box=pic.clip(box);
    const int h=box.h();
    const auto c=pic.cut(box);
    if(h<6||box.w()<4||c.w()<=0||c.h()<=0)return std::nullopt;
    const int w=c.w(),ch=c.h();
    std::vector<int> bright(size_t(w)*ch);
    for(int y=0;y<ch;++y)for(int x=0;x<w;++x){const auto* p=pic.at(c.x0+x,c.y0+y);bright[size_t(y)*w+x]=std::max({p[0],p[1],p[2]});}
    const int k=std::max(3,int(0.45*h)|1);
    std::vector<int> strokes(bright.size());
    if(dark){
        const auto closed=morph(morph(bright,w,ch,k,false),w,ch,k,true);
        for(size_t i=0;i<bright.size();++i)strokes[i]=closed[i]-bright[i];
    }else{
        const auto opened=morph(morph(bright,w,ch,k,true),w,ch,k,false);
        for(size_t i=0;i<bright.size();++i)strokes[i]=bright[i]-opened[i];
    }
    const int y1=std::min(ch,c.dy+h),x1=std::min(w,c.dx+box.w());
    std::vector<float> inside;
    for(int y=c.dy;y<y1;++y)for(int x=c.dx;x<x1;++x)inside.push_back(float(strokes[size_t(y)*w+x]));
    if(inside.empty())return std::nullopt;
    const float strong=percentile(inside,97);
    if(strong<35)return std::nullopt;
    std::vector<Rgb> picked;
    for(int y=c.dy;y<y1;++y)for(int x=c.dx;x<x1;++x)
        if(float(strokes[size_t(y)*w+x])>=0.6f*strong){const auto* p=pic.at(c.x0+x,c.y0+y);picked.push_back({float(p[0]),float(p[1]),float(p[2])});}
    return medianColour(picked);
}

// The most colourful colour in the box, if anything in it has a colour at all. (An orange name
// over a white wall is darker than the wall; its middle is still the most colourful thing.)
std::optional<Rgb> vividColour(Pic& pic,Box box){
    box=pic.clip(box);
    if(box.w()<=0||box.h()<=0)return std::nullopt;
    std::vector<float> spread;spread.reserve(size_t(box.w())*box.h());
    for(int y=box.t;y<box.b;++y)for(int x=box.l;x<box.r;++x){const auto* p=pic.at(x,y);spread.push_back(float(std::max({p[0],p[1],p[2]})-std::min({p[0],p[1],p[2]})));}
    const float strong=percentile(spread,95);
    if(strong<40)return std::nullopt;
    std::vector<Rgb> picked;size_t i=0;
    for(int y=box.t;y<box.b;++y)for(int x=box.l;x<box.r;++x,++i)
        if(spread[i]>=0.7f*strong){const auto* p=pic.at(x,y);picked.push_back({float(p[0]),float(p[1]),float(p[2])});}
    return medianColour(picked);
}

// Letters of a colour in the box. A pixel is of the colour when it is nearer to it than to what
// lies behind the text there (so a plate's white is not "white letters"); a letter is a patch of
// it that stays within the line and is no wider than a few of the letters read (letters: how many
// characters were read in the box). byHue: for coloured letters, which video packing smears into
// paler shades of the same hue.
Letters lettersOf(Pic& pic,Box box,const Rgb& colour,size_t letters,bool byHue=false){
    Letters out;
    box=pic.clip(box);
    const int h=box.h();
    const auto c=pic.cut(box);
    if(h<6||c.w()<=0||c.h()<=0)return out;
    const int w=c.w(),ch=c.h();
    const auto& behind=pic.behind(c,std::min(99,std::max(5,int(1.3*h)|1)));
    const int want[3]={int(colour[0]),int(colour[1]),int(colour[2])};
    auto off=[&](int r,int g,int b){return std::max({std::abs(r-want[0]),std::abs(g-want[1]),std::abs(b-want[2])});};
    std::vector<uint8_t> near(size_t(w)*ch);
    const float hue=hueDeg(colour)/2.0f;
    const float least=std::max(25.0f,0.45f*(std::max({colour[0],colour[1],colour[2]})-std::min({colour[0],colour[1],colour[2]})));
    auto ofHue=[&](int r,int g,int b){
        const int mx=std::max({r,g,b}),mn=std::min({r,g,b});
        if(float(mx-mn)<least)return false;
        const float ph=std::round(hueDeg(float(r),float(g),float(b))/2.0f);
        const float apart=std::abs(ph-hue);
        return std::min(apart,180.0f-apart)<=14.0f;
    };
    for(int y=0;y<ch;++y)for(int x=0;x<w;++x){
        const auto* p=pic.at(c.x0+x,c.y0+y);
        const uint8_t* q=&behind[(size_t(y)*w+x)*3];
        const int unlike=off(q[0],q[1],q[2]),mine=off(p[0],p[1],p[2]);
        near[size_t(y)*w+x]=byHue?(ofHue(p[0],p[1],p[2])&&(!ofHue(q[0],q[1],q[2])||mine<0.6*unlike))
                                 :(mine<=kTolerance&&mine<0.5*unlike);
    }
    // Patches of the colour, 8-connected.
    struct Patch { int x0,y0,x1,y1,area; };
    std::vector<Patch> patches;
    std::vector<int> stack;
    for(int y=0;y<ch;++y)for(int x=0;x<w;++x){
        if(near[size_t(y)*w+x]!=1)continue;
        Patch pa{x,y,x,y,0};
        near[size_t(y)*w+x]=2;stack.push_back(y*w+x);
        while(!stack.empty()){
            const int i=stack.back();stack.pop_back();
            const int px=i%w,py=i/w;
            ++pa.area;pa.x0=std::min(pa.x0,px);pa.x1=std::max(pa.x1,px);pa.y0=std::min(pa.y0,py);pa.y1=std::max(pa.y1,py);
            for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){
                const int nx=px+dx,ny=py+dy;
                if(nx<0||ny<0||nx>=w||ny>=ch||near[size_t(ny)*w+nx]!=1)continue;
                near[size_t(ny)*w+nx]=2;stack.push_back(ny*w+nx);
            }
        }
        patches.push_back(pa);
    }
    const double widest=byHue?double(w):letters?std::max(3.0*box.w()/double(letters),0.3*h):2.5*h;
    std::vector<Patch> kept;
    for(const auto& p:patches){
        const int wide=p.x1-p.x0+1,tall=p.y1-p.y0+1;
        if(p.y0>0&&p.y0+tall<ch&&tall<=1.1*h&&wide<=widest&&p.area>=3)kept.push_back(p);
    }
    std::vector<uint8_t> columns(size_t(w),0);
    auto slice=[&]{
        const int from=std::min(c.dx,w),to=std::min(w,c.dx+box.w());
        out.cols.assign(columns.begin()+from,columns.begin()+std::max(from,to));
    };
    if(kept.empty()){slice();return out;}
    // As tall as most of what is painted in the colour (a stray streak is little of it).
    std::vector<std::pair<int,int>> byTall;for(const auto& p:kept)byTall.push_back({p.y1-p.y0+1,p.area});
    std::stable_sort(byTall.begin(),byTall.end(),[](const auto& a,const auto& b){return a.first<b.first;});
    long total=0;for(const auto& t:byTall)total+=t.second;
    long run=0;float letter=float(byTall.back().first);
    for(const auto& t:byTall){run+=t.second;if(double(run)>=0.8*double(total)){letter=float(t.first);break;}}
    for(const auto& p:kept){
        const int wide=p.x1-p.x0+1,tall=p.y1-p.y0+1;
        if(float(tall)>=0.2f*letter||float(wide)>=0.2f*letter)for(int x=p.x0;x<=p.x1;++x)columns[size_t(x)]=1;
        if(float(tall)>=0.45f*letter)out.mids.push_back(float(p.x0)+float(wide)/2.0f-float(c.dx));
    }
    out.height=letter;
    slice();
    return out;
}

// Are these words of the piece written in this colour? Letters of it along most of their width,
// and about one letter-high patch per letter read (real lines measured 0.71-0.78 of the width and
// one patch a letter; the white between a plate's dark letters at times half the width, rarely
// the patches).
bool backed(Pic& pic,const Piece& p,const Rgb& colour,const std::vector<PieceWord>& words,
            const Letters* found=nullptr,bool counted=true){
    const Box box=pic.clip(boxOf(p));
    Letters own;
    if(!found){own=lettersOf(pic,box,colour,p.text.size());found=&own;}
    if(found->height<0.2f*float(box.h()))return false; // specks of the colour, not letters
    long width=0,within=0,patches=0,letters=0;
    for(const auto& wd:words){
        const int a=std::max(0,int(wd.left)-box.l),b=std::min(box.w(),int(std::ceil(wd.right))-box.l);
        if(b-a<2)continue; // (a line's last word is at times placed far outside it)
        width+=b-a;
        for(int x=a;x<b&&x<int(found->cols.size());++x)within+=found->cols[size_t(x)];
        for(float m:found->mids)patches+=(m>=float(a)&&m<=float(b));
        letters+=long(alnumCount(wd.text));
    }
    if(!width)return false;
    return double(within)>=kSupported*double(width)&&(!counted||double(patches)>=kPatches*double(letters));
}

// These words of the piece as a piece of their own.
std::optional<Piece> partOfPiece(const Piece& p,const std::vector<PieceWord>& run){
    const Box b=boxOf(p);
    const int l=std::max(b.l,int(run.front().left)),r=std::min(b.r,int(std::ceil(run.back().right)));
    if(r-l<4)return std::nullopt;
    Piece o;
    for(const auto& w:run){if(!o.text.empty())o.text+=L' ';o.text+=w.text;}
    o.score=p.score;
    o.quad={{{float(l),float(b.t)},{float(r),float(b.t)},{float(r),float(b.b)},{float(l),float(b.b)}}};
    o.words=run;
    return o;
}

size_t letterCount(const std::wstring& s,size_t* lower=nullptr){
    size_t n=0,lo=0;
    for(wchar_t c:s)if(isLetter(c)){++n;lo+=isLower(c);}
    if(lower)*lower=lo;
    return n;
}
// Speech: words in small letters read with confidence, no numbers.
bool readsAsSpeech(const Piece& p){
    size_t lower=0;const size_t letters=letterCount(p.text,&lower);
    return p.score>=0.85f&&splitWords(p.text).size()>=2&&letters>=8&&std::none_of(p.text.begin(),p.text.end(),isDigit)
        &&double(lower)>=0.6*double(letters);
}
// Right above, below or next to the line, in letters of about its size.
bool besideOf(const Piece& p,const Piece& line){
    const Box a=boxOf(p),b=boxOf(line);
    const double ratio=double(a.h())/std::max(b.h(),1);
    if(ratio<0.6||ratio>1.6)return false;
    const int apart=std::max(a.t-b.b,b.t-a.b),aside=std::max(a.l-b.r,b.l-a.r);
    return apart<=0.6*b.h()&&aside<=(apart<0?2*b.h():0);
}
// On the line's own row, or set over its middle or its start.
bool inLineWith(const Piece& p,const Piece& line){
    const Box a=boxOf(p),b=boxOf(line);
    if(std::max(a.t-b.b,b.t-a.b)<-0.5*std::min(a.h(),b.h()))return true; // one line read as two pieces
    return std::abs((a.l+a.r)-(b.l+b.r))/2.0<=b.h()||std::abs(a.l-b.l)<=0.6*b.h();
}
} // namespace

bool sentenceLike(const Piece& p){
    size_t lower=0;const size_t letters=letterCount(p.text,&lower);
    const size_t words=splitWords(p.text).size();
    if(p.score>=0.9f&&words>=3&&letters>=12)return true;
    return p.score>=0.8f&&words>=2&&letters>=8&&double(lower)>=0.6*double(letters);
}

void SubtitleStyle::moved(){places_.clear();rows_.clear();learntFrom_.clear();namedMiddles_.clear();}
void SubtitleStyle::reset(){
    moved();colour_.reset();dark_=false;letter_.reset();samples_.clear();whiteHere_.clear();namedHeights_.clear();
}

// Text that is no line by itself: no letters, numbers rather than words, or (Russian subtitles)
// Latin letters only, unless they are look-alikes that make a Russian word ("Het!", not "Ct").
bool SubtitleStyle::weak(const Piece& p)const{
    size_t letters=0,digits=0;bool latinOnly=true,looking=true;
    for(wchar_t c:p.text){
        digits+=isDigit(c);
        if(!isLetter(c))continue;
        ++letters;
        if(uint32_t(c)>=128)latinOnly=false;
        else looking&=kRussianLooking.find(c)!=std::wstring_view::npos;
    }
    if(!letters||(digits>=2&&digits>=letters))return true;
    if(russian_&&latinOnly){
        if(!looking)return true;
        const auto fixed=cyrillicLookalikes(p.text,words_);
        return std::all_of(fixed.begin(),fixed.end(),[](wchar_t c){return !isLetter(c)||uint32_t(c)<128;});
    }
    return false;
}

void SubtitleStyle::learn(Picture& pic,const Piece& p){
    if(!sentenceLike(p))return;
    const auto text=plain(p.text,false);
    for(const auto& seen:learntFrom_)if(ratio(text,seen)>0.8)return; // the same line again says nothing new
    const Box box=boxOf(p);
    learntFrom_.push_back(text);if(learntFrom_.size()>8)learntFrom_.pop_front();
    // Where lines stand is learnt from every sentence, whatever is behind it.
    places_.push_back({float(box.t+box.b)/2.0f,float(box.l+box.r)/2.0f,float(box.l)});if(places_.size()>40)places_.pop_front();
    for(bool dark:{false,true}){ // light letters first: nearly every game has them
        // Each word's own colour; the colour the line ends in is the lines' (a name begins a
        // line, never ends it, so a pale name cannot drift the learnt white).
        std::vector<std::optional<Rgb>> colours;
        for(const auto& wd:p.words){
            const auto part=partOfPiece(p,{wd});
            colours.push_back(part?ownColour(pic,boxOf(*part),dark):std::nullopt);
        }
        std::optional<Rgb> last;
        for(auto it=colours.rbegin();it!=colours.rend();++it)if(*it){last=*it;break;}
        if(!last)continue;
        std::vector<Rgb> alike;
        for(const auto& c:colours)if(c&&maxDiff(*c,*last)<=30)alike.push_back(*c);
        if(alike.size()<2||double(alike.size())<0.5*double(p.words.size()))continue;
        const Rgb colour=medianColour(alike);
        const auto found=lettersOf(pic,box,colour,p.text.size());
        size_t written=0;
        for(const auto& wd:p.words)written+=backed(pic,p,colour,{wd},&found);
        if(written>=2&&double(written)>=0.5*double(p.words.size())){
            samples_.push_back({colour,dark,found.height});if(samples_.size()>24)samples_.pop_front();
            settle();
            return;
        }
    }
}

// The style most remembered lines share. Dark letters count only while no light line was seen
// (white lines over a white wall show by their shadow alone).
void SubtitleStyle::settle(){
    std::vector<Sample> among;
    for(const auto& s:samples_)if(!s.dark)among.push_back(s);
    if(among.empty()&&samples_.size()>=3)among.assign(samples_.begin(),samples_.end());
    std::vector<Sample> best;
    for(const auto& s:among){
        std::vector<Sample> alike;
        for(const auto& o:among)if(o.dark==s.dark&&maxDiff(o.colour,s.colour)<=30)alike.push_back(o);
        if(alike.size()>=best.size())best=std::move(alike);
    }
    if(best.empty())return;
    std::vector<Rgb> cs;std::vector<float> ls;
    for(const auto& s:best){cs.push_back(s.colour);ls.push_back(s.letter);}
    colour_=medianColour(cs);dark_=best.front().dark;letter_=median(ls);
}

// Where this game's lines stand: on one of their rows, centred like them or starting where they start.
bool SubtitleStyle::inPlace(const Piece& p)const{
    const Box b=boxOf(p);
    const float h=float(b.h()),mid=float(b.t+b.b)/2.0f,centre=float(b.l+b.r)/2.0f;
    return std::any_of(places_.begin(),places_.end(),[&](const Place& pl){
        return std::abs(mid-pl.row)<=0.3f*h&&(std::abs(centre-pl.middle)<=0.6f*h||std::abs(float(b.l)-pl.left)<=0.4f*h);
    });
}

// The rows lines stand on, once enough were seen and they keep to a few (none: unknown, or a game
// that puts its lines by the speaker).
std::optional<std::vector<float>> SubtitleStyle::knownRows()const{
    if(places_.size()<6||!letter_)return std::nullopt;
    std::vector<float> distinct;
    for(size_t i=places_.size()>16?places_.size()-16:0;i<places_.size();++i){
        const float row=places_[i].row;
        if(std::none_of(distinct.begin(),distinct.end(),[&](float k){return std::abs(row-k)<=0.6f**letter_;}))distinct.push_back(row);
    }
    if(distinct.size()>3)return std::nullopt;
    std::vector<float> rows;
    for(const auto& pl:places_)rows.push_back(pl.row);
    rows.insert(rows.end(),rows_.begin(),rows_.end());
    return rows;
}

// Text that is no line by itself, as a part of this line: on its row beside its words, or its
// next row right under or over it, set like it, in letters of its colour and size.
bool SubtitleStyle::partOf(Picture& pic,const Piece& p,const Piece& line)const{
    const Box a=boxOf(p),b=boxOf(line);
    const double ratio=double(a.h())/std::max(b.h(),1);
    if(ratio<0.6||ratio>1.6)return false;
    const int apart=std::max(a.t-b.b,b.t-a.b),aside=std::max(a.l-b.r,b.l-a.r);
    if(apart<-0.5*std::min(a.h(),b.h()))return aside<=2*b.h();
    if(apart>0.6*b.h()||aside>0||!inLineWith(p,line))return false;
    if(!colour_||dark_)return true;
    const auto found=lettersOf(pic,a,*colour_,p.text.size());
    return backed(pic,p,*colour_,p.words,&found,false)&&letter_&&*letter_>0
        &&found.height/ *letter_>=0.7f&&found.height/ *letter_<=1.4f;
}

std::vector<SortedPiece> SubtitleStyle::sort(const Crop& crop,std::vector<Piece> pieces){
    Picture pic(crop);
    if(!pic.ok())return {};
    const size_t n=pieces.size();
    std::vector<char> isWeak(n);
    for(size_t i=0;i<n;++i)isWeak[i]=weak(pieces[i]);
    for(size_t i=0;i<n;++i){
        auto& p=pieces[i];
        if(p.words.empty()){const Box b=boxOf(p);p.words={{p.text,float(b.l),float(b.r)}};}
        if(!isWeak[i])learn(pic,p); // (not from a sign in English)
    }
    std::vector<size_t> lines;
    for(size_t i=0;i<n;++i)if(!isWeak[i]&&sentenceLike(pieces[i]))lines.push_back(i);
    // Before anything is learnt: a piece beside one that is all white is told against that white
    // (only beside it: a white sign elsewhere must not turn yellow lines into marks).
    whiteHere_.clear();
    if(!colour_)for(size_t i=0;i<n;++i){
        const auto light=ownColour(pic,boxOf(pieces[i]));
        if(light&&saturation(*light)<kSaturated&&backed(pic,pieces[i],*light,pieces[i].words))whiteHere_.push_back({i,*light});
    }
    struct Found { size_t i; std::vector<WordKind> kinds; bool beside; };
    std::vector<Found> found;
    auto hasBody=[](const std::vector<WordKind>& k){return std::find(k.begin(),k.end(),WordKind::Body)!=k.end();};
    for(size_t i=0;i<n;++i){
        if(isWeak[i])continue;
        const bool beside=std::any_of(lines.begin(),lines.end(),[&](size_t j){return j!=i&&besideOf(pieces[i],pieces[j]);});
        if(auto k=kinds(pic,pieces,i,beside))found.push_back({i,std::move(*k),beside});
    }
    // Text that is no line by itself is kept as a part of one (and Latin text that reads as speech
    // standing exactly where this game's lines stand: English spoken in a Russian translation).
    std::vector<size_t> spoken;
    for(const auto& f:found)if(hasBody(f.kinds))spoken.push_back(f.i);
    for(size_t i=0;i<n;++i){
        if(!isWeak[i])continue;
        const bool part=std::any_of(spoken.begin(),spoken.end(),[&](size_t j){return partOf(pic,pieces[i],pieces[j]);});
        if(!part&&!(colour_&&readsAsSpeech(pieces[i])&&inPlace(pieces[i])))continue;
        if(auto k=kinds(pic,pieces,i,part))found.push_back({i,std::move(*k),part});
    }
    std::vector<SortedPiece> kept;
    for(const auto& f:found){
        const auto& p=pieces[f.i];
        // Nothing of it read and no line beside it with this over its middle or start: coloured
        // text in the scene, not a speaker's name.
        if(!hasBody(f.kinds)&&std::none_of(found.begin(),found.end(),[&](const Found& o){
               return o.i!=f.i&&hasBody(o.kinds)&&besideOf(p,pieces[o.i])&&inLineWith(p,pieces[o.i]);}))continue;
        kept.push_back({p,f.kinds});
        const Box b=boxOf(p);const float mid=float(b.t+b.b)/2.0f;
        if(f.beside&&std::find(lines.begin(),lines.end(),f.i)==lines.end()
           &&std::none_of(rows_.begin(),rows_.end(),[&](float r){return std::abs(mid-r)<=4;})){
            rows_.push_back(mid);
            if(rows_.size()>12)rows_.erase(rows_.begin());
        }
    }
    return kept;
}

std::optional<std::vector<WordKind>> SubtitleStyle::kinds(Picture& pic,std::vector<Piece>& pieces,size_t index,bool beside){
    Piece& p=pieces[index];
    const Box box=boxOf(p);
    const int h=box.h(),w=box.w();
    const std::vector<WordKind> everything(p.words.size(),WordKind::Body);
    if(h<6||w<4)return everything;
    // A sentence is read whatever it looks like; so is a short piece where lines stand or right
    // beside a line (over a white wall a white line shows by its shadow alone).
    const bool sure=sentenceLike(p)||beside||inPlace(p);
    if(!sure){
        const float along=std::hypot(p.quad[1][0]-p.quad[0][0],p.quad[1][1]-p.quad[0][1]);
        const float up=std::hypot(p.quad[3][0]-p.quad[0][0],p.quad[3][1]-p.quad[0][1]);
        if(along>1.5f*up&&p.lean()>kLevel)return std::nullopt; // painted on something in the scene
        const auto rows=knownRows();
        const float mid=float(box.t+box.b)/2.0f;
        if(rows&&std::none_of(rows->begin(),rows->end(),[&](float r){return std::abs(mid-r)<=0.35f*float(h);}))
            return std::nullopt; // not on a row the lines stand on
    }
    std::optional<Rgb> body=colour_;
    if(!body){
        // Nothing learnt: against the piece's own white words, or a white line beside it...
        body=ownWhite(pic,p);
        if(!body)for(const auto& [j,white]:whiteHere_)if(j!=index&&besideOf(p,pieces[j])){body=white;break;}
        if(!body){
            // ...or whatever has light letters is a line.
            const auto light=ownColour(pic,box);
            const bool ok=light&&backed(pic,p,*light,p.words);
            if(sure||ok)return everything;
            return std::nullopt;
        }
    }
    const auto found=lettersOf(pic,box,*body,p.text.size());
    if(colour_){
        if(!sure&&letter_&&found.height>0&&(found.height/ *letter_<0.6f||found.height/ *letter_>1.6f))
            return std::nullopt; // the lines' colour but far from their size: a title, a sign
        if(dark_){
            if(sure||backed(pic,p,*body,p.words,&found))return everything;
            return std::nullopt;
        }
    }
    splitWords(pic,p,*body,found.cols);
    Kinds k(p.words.size());
    for(size_t i=0;i<p.words.size();++i)if(backed(pic,p,*body,{p.words[i]},&found))k[i]=WordKind::Body;
    paleName(pic,p,*body,k);
    // The other words one by one: another colour, a slightly different white, or nothing.
    for(size_t i=0;i<p.words.size();++i){
        if(k[i])continue;
        const std::vector<PieceWord> run{p.words[i]};
        const auto part=partOfPiece(p,run);
        if(!part)continue;
        const Box pb=boxOf(*part);
        if(alnumCount(p.words[i].text)>=2){
            for(int attempt=0;attempt<2&&!k[i];++attempt){
                const auto vivid=attempt==0?ownColour(pic,pb):vividColour(pic,pb);
                if(!vivid||!coloured(*vivid,body))continue;
                // Letters of that colour along the word, no smaller than the lines' (an awning's
                // red stripe behind a white word is not letters).
                const auto written=lettersOf(pic,pb,*vivid,part->text.size(),true);
                const float ref=letter_&&*letter_>0?*letter_:found.height>0?found.height:written.height;
                if(backed(pic,*part,*vivid,run,&written,false)&&written.height>=0.5f*ref)k[i]=WordKind::Marked;
            }
        }
        if(!k[i]){
            const auto light=ownColour(pic,pb);
            if(!light)continue;
            const auto written=lettersOf(pic,pb,*light,part->text.size());
            if(!backed(pic,*part,*light,run,&written)||coloured(*light,body))continue;
            if(std::max({(*light)[0],(*light)[1],(*light)[2]})<0.75f*std::max({(*body)[0],(*body)[1],(*body)[2]}))continue;
            const float ref=letter_&&*letter_>0?*letter_:written.height;
            // A white a little unlike the lines' (a line fading in), in letters of their size.
            if(ref>0&&written.height/ref>=0.6f&&written.height/ref<=1.6f)k[i]=WordKind::Body;
        }
    }
    const bool anyBody=std::any_of(k.begin(),k.end(),[](const auto& x){return x==WordKind::Body;});
    if(!anyBody&&!sure){
        if(std::none_of(k.begin(),k.end(),[](const auto& x){return x==WordKind::Marked;}))
            return std::nullopt; // no letters of a subtitle at all: a sign, a plate
        std::vector<WordKind> out;
        for(const auto& x:k)out.push_back(x==WordKind::Marked?WordKind::Marked:WordKind::Scene);
        return out;
    }
    // A name runs up to its colon ("Police officer:" with only "Police" told apart), unless the
    // marked words end in one already ("Cal: Tell me honestly: ...").
    size_t lead=0;while(lead<k.size()&&k[lead]==WordKind::Marked)++lead;
    if(lead&&!endsInColon(p.words[lead-1].text))
        for(size_t e=lead;e<std::min(lead+2,k.size());++e)
            if(endsInColon(p.words[e].text)){for(size_t j=lead;j<=e;++j)k[j]=WordKind::Marked;break;}
    std::vector<WordKind> out;
    for(const auto& x:k)out.push_back(x?*x:WordKind::Body); // untold words beside line words: read
    return out;
}

// Leading words in a pale colour of their own pass for the lines' white; looked at apart they
// have a colour and the words after them do not. A whole line tinted by what is behind stays.
void SubtitleStyle::paleName(Picture& pic,const Piece& p,const Rgb& body,Kinds& k)const{
    std::vector<std::optional<Rgb>> colours;
    for(const auto& wd:p.words){
        const auto part=partOfPiece(p,{wd});
        colours.push_back(part?ownColour(pic,boxOf(*part)):std::nullopt);
    }
    size_t lead=0;
    for(;lead<p.words.size();++lead){
        const auto& c=colours[lead];
        if(k[lead]!=WordKind::Body||!c||!coloured(*c,body)||maxDiff(*c,body)<20||alnumCount(p.words[lead].text)<2)break;
    }
    std::vector<Rgb> rest;
    for(size_t i=lead;i<colours.size();++i)if(colours[i])rest.push_back(*colours[i]);
    if(!lead||rest.empty())return;
    const auto tinted=std::count_if(rest.begin(),rest.end(),[&](const Rgb& c){return coloured(c,body);});
    if(double(tinted)>0.25*double(rest.size()))return;
    const std::vector<PieceWord> run(p.words.begin(),p.words.begin()+long(lead));
    const auto part=partOfPiece(p,run);
    if(!part)return;
    std::vector<Rgb> leadColours;for(size_t i=0;i<lead;++i)leadColours.push_back(*colours[i]);
    const Rgb colour=medianColour(leadColours);
    const auto written=lettersOf(pic,boxOf(*part),colour,part->text.size(),true);
    if(backed(pic,*part,colour,run,&written,false))for(size_t i=0;i<lead;++i)k[i]=WordKind::Marked;
}

// The colour of the piece's white words, when it has such words and others besides.
std::optional<Rgb> SubtitleStyle::ownWhite(Picture& pic,const Piece& p)const{
    if(p.words.size()<2)return std::nullopt;
    std::vector<Rgb> whites;
    for(const auto& wd:p.words){
        const auto part=partOfPiece(p,{wd});
        if(!part||alnumCount(wd.text)<2)continue;
        const auto light=ownColour(pic,boxOf(*part));
        if(light&&saturation(*light)<kSaturated&&backed(pic,*part,*light,{wd}))whites.push_back(*light);
    }
    if(whites.empty()||whites.size()>=p.words.size())return std::nullopt;
    return medianColour(whites);
}

// "Franklin:Run!" read as one word, the name in another colour: cut where the colours change,
// if that is where the colon is.
void SubtitleStyle::splitWords(Picture& pic,Piece& p,const Rgb& body,const std::vector<uint8_t>& bodyCols)const{
    const int left=pic.clip(boxOf(p)).l;
    std::vector<PieceWord> words;
    for(const auto& wd:p.words){
        words.push_back(wd);
        const auto part=partOfPiece(p,{wd});
        const auto c1=wd.text.find(L':'),c2=wd.text.find(L'：');
        const long where=std::max(c1==std::wstring::npos?-1L:long(c1),c2==std::wstring::npos?-1L:long(c2));
        if(!part||where<2||where>=long(wd.text.size())-1)continue; // only "name:answer"
        const Box pb=pic.clip(boxOf(*part));
        const int a=std::clamp(pb.l-left,0,int(bodyCols.size())),b=std::clamp(a+pb.w(),a,int(bodyCols.size()));
        const std::vector<uint8_t> own(bodyCols.begin()+a,bodyCols.begin()+b);
        if(own.empty())continue;
        const auto ownSum=std::count(own.begin(),own.end(),1);
        if(double(ownSum)<0.15*double(own.size())||double(ownSum)>0.85*double(own.size()))continue; // all one colour
        const auto vivid=vividColour(pic,pb);
        if(!vivid||!coloured(*vivid,body))continue;
        const auto other=lettersOf(pic,pb,*vivid,wd.text.size(),true).cols;
        if(other.size()!=own.size()||std::none_of(other.begin(),other.end(),[](uint8_t v){return v;}))continue;
        // The cut with most of the other colour before it and most of the lines' own after it.
        long gain=0,bestGain=-(1L<<30);size_t cut=0;
        for(size_t i=0;i<own.size();++i){gain+=long(other[i])-long(own[i]);if(gain>bestGain){bestGain=gain;cut=i+1;}}
        auto mean=[](auto from,auto to){const long n=long(to-from);return n?double(std::count(from,to,1))/double(n):0.0;};
        if(mean(other.begin(),other.begin()+long(cut))<kSupported||cut>=own.size()
           ||mean(own.begin()+long(cut),own.end())<kSupported)continue;
        const long at=where+1;
        if(std::abs(double(at)-double(wd.text.size())*double(cut)/double(own.size()))>2.5)continue; // not at the colon
        const float middle=float(pb.l)+float(cut);
        words.back()={wd.text.substr(0,size_t(at)),wd.left,middle};
        words.push_back({wd.text.substr(size_t(at)),middle,wd.right});
    }
    p.words=std::move(words);
}

std::vector<std::vector<size_t>> pieceRows(const std::vector<Piece>& pieces){
    std::vector<size_t> order(pieces.size());
    for(size_t i=0;i<order.size();++i)order[i]=i;
    std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b){
        const auto ba=pieces[a].box(),bb=pieces[b].box();return ba.top+ba.bottom<bb.top+bb.bottom;});
    std::vector<std::vector<size_t>> rows;
    for(size_t i:order){
        const auto b=pieces[i].box();const float centre=float(b.top+b.bottom)/2.0f;
        if(!rows.empty()){
            const auto first=pieces[rows.back().front()].box();
            if(float(first.top)<=centre&&centre<=float(first.bottom)){rows.back().push_back(i);continue;}
        }
        rows.push_back({i});
    }
    for(auto& r:rows)std::stable_sort(r.begin(),r.end(),[&](size_t a,size_t b){return pieces[a].box().left<pieces[b].box().left;});
    return rows;
}

namespace {
// What of a piece is spoken: with the leading words in another colour and without them, those
// words, and whether a colon parts them from the rest.
struct Spoken { std::wstring whole,rest; std::vector<std::wstring> marked; bool colon=false; };
std::wstring trimmed(const std::wstring& s){
    size_t a=0,b=s.size();while(a<b&&isSpace(s[a]))++a;while(b>a&&isSpace(s[b-1]))--b;return s.substr(a,b-a);
}
Spoken spokenOf(const Piece& p,const std::vector<WordKind>& k){
    size_t first=0;while(first<k.size()&&k[first]!=WordKind::Body)++first;
    std::vector<size_t> lead;
    for(size_t i=0;i<first;++i)if(k[i]==WordKind::Marked)lead.push_back(i);
    if(lead.empty()&&std::none_of(k.begin(),k.end(),[](WordKind x){return x==WordKind::Scene;}))
        return {trimmed(p.text),trimmed(p.text),{},false};
    // Words are found in the text as read, so what stays keeps its own spacing.
    std::vector<std::pair<size_t,size_t>> spans;bool located=true;size_t at=0;
    for(const auto& wd:p.words){
        const auto f=p.text.find(wd.text,at);
        if(f==std::wstring::npos){located=false;break;}
        spans.push_back({f,f+wd.text.size()});at=f+wd.text.size();
    }
    auto keep=[&](bool dropLead){
        auto dropped=[&](size_t i){return k[i]==WordKind::Scene||(dropLead&&std::find(lead.begin(),lead.end(),i)!=lead.end());};
        if(!located){
            std::wstring o;
            for(size_t i=0;i<p.words.size();++i)if(!dropped(i)){if(!o.empty())o+=L' ';o+=p.words[i].text;}
            return o;
        }
        std::wstring t=p.text;
        for(size_t i=0;i<spans.size();++i)if(dropped(i))for(size_t j=spans[i].first;j<spans[i].second;++j)t[j]=L' ';
        return squeeze(t);
    };
    Spoken s;
    s.whole=keep(false);
    auto rest=keep(true);
    for(size_t i:lead)s.marked.push_back(p.words[i].text);
    // The colon's edge read as one or two letters more: "Jason:I", "Jason:l", "Jason:1".
    if(!s.marked.empty()){
        auto& w=s.marked.back();
        size_t e=w.size(),n=0;
        while(e&&n<2&&std::wstring_view(L"IlІі|!1").find(w[e-1])!=std::wstring_view::npos){--e;++n;}
        if(n&&e&&(w[e-1]==L':'||w[e-1]==L'：'))w.resize(e);
    }
    size_t r=0;while(r<rest.size()&&isSpace(rest[r]))++r;
    s.colon=!s.marked.empty()&&(endsInColon(s.marked.back())||(r<rest.size()&&(rest[r]==L':'||rest[r]==L'：')));
    // What joined the name to the line ("NAME: text", "NAME - text") goes with the name.
    while(r<rest.size()&&std::wstring_view(L":：-–— ").find(rest[r])!=std::wstring_view::npos)++r;
    s.rest=trimmed(rest.substr(r));
    return s;
}
} // namespace

// The speaker's name: the words in another colour that begin the subtitle (on its first row or a
// row of their own above it). Elsewhere such words are spoken, except at the start of a later row
// before a colon: the second speaker of a two-speaker subtitle.
SubtitleText linesAndName(const std::vector<SortedPiece>& sorted){
    std::vector<Piece> pieces;pieces.reserve(sorted.size());
    for(const auto& s:sorted)pieces.push_back(s.piece);
    SubtitleText out;
    bool begun=false;
    for(const auto& row:pieceRows(pieces)){
        std::vector<std::wstring> parts,lead;
        for(size_t i:row){
            auto s=spokenOf(sorted[i].piece,sorted[i].kinds);
            if(!s.marked.empty()&&parts.empty()&&(!begun||s.colon)){
                lead.insert(lead.end(),s.marked.begin(),s.marked.end());
                s.whole=s.rest;
            }
            if(!s.whole.empty())parts.push_back(s.whole);
        }
        if(!lead.empty()&&!begun){
            for(const auto& l:lead){if(!out.name.empty())out.name+=L' ';out.name+=l;}
        }
        if(!lead.empty()||!parts.empty())begun=true;
        if(!parts.empty()){
            std::wstring line;
            for(const auto& pt:parts){if(!line.empty())line+=L' ';line+=pt;}
            out.lines.push_back(line);
        }
    }
    return out;
}

// --- a game's own rules ----------------------------------------------------------------------
// GTA VI, measured on half an hour of 1080p play : every subtitle starts with the
// speaker's name in a colour and a colon; its one to three rows are centred on one spot, a row
// every 48 px; the next speaker's subtitle comes 58 px lower, the mission line 66 px under the last
// row, unnamed. So a subtitle is a row that begins with a name, plus the rows one step under it.
namespace {
constexpr float kRowStep=1.07f;              // row to row, in row-box heights
constexpr float kStepLow=0.8f,kStepHigh=1.19f; // how far from that a row may be (the mission line is at ~1.37)
float medianOf(const std::deque<float>& d){return median(std::vector<float>(d.begin(),d.end()));}
std::wstring joinWords(const std::vector<std::wstring>& v){
    std::wstring o;
    for(const auto& w:v){if(w.empty())continue;if(!o.empty())o+=L' ';o+=w;}
    return o;
}
} // namespace

// The name a row begins with, if it does. Told by colour when it can be; else by how it is
// written (pale tints over sunlit asphalt are not told apart): one to four words before a colon,
// the first capitalised, no sentence marks or digits, speech after it, and the row centred where
// rows with a coloured name were.
std::optional<std::wstring> SubtitleStyle::named(std::vector<SortedPiece*>& row,float across,float tall){
    auto& first=*row.front();
    const auto s=spokenOf(first.piece,first.kinds);
    if(!s.marked.empty()&&s.colon&&(!s.rest.empty()||row.size()>1)){
        namedMiddles_.push_back(across);if(namedMiddles_.size()>30)namedMiddles_.pop_front();
        std::vector<std::wstring> m=s.marked;
        return joinWords(m);
    }
    if(namedMiddles_.size()>=3&&std::abs(across-medianOf(namedMiddles_))>0.9f*tall)return std::nullopt;
    auto& words=first.piece.words;
    size_t end=0;
    while(end<std::min<size_t>(4,words.size())&&words[end].text.find_first_of(L":：")==std::wstring::npos)++end;
    if(end>=std::min<size_t>(4,words.size()))return std::nullopt;
    {
        const PieceWord last=words[end];
        const size_t at=last.text.find_last_of(L":：");
        if(at+1<last.text.size()){
            // "name:answer" read as one word: two, parted at the colon.
            const float cut=last.left+(last.right-last.left)*float(at+1)/float(last.text.size());
            words[end]={last.text.substr(0,at+1),last.left,cut};
            words.insert(words.begin()+long(end)+1,PieceWord{last.text.substr(at+1),cut,last.right});
            first.kinds.insert(first.kinds.begin()+long(end)+1,first.kinds[end]);
        }
    }
    std::wstring name;
    for(size_t i=0;i<=end;++i){if(i)name+=L' ';name+=words[i].text;}
    if(name.empty()||!isUpper(name[0])||name.find_first_of(L".,!?…;")!=std::wstring::npos
       ||std::any_of(name.begin(),name.end(),isDigit)||!(words.size()>end+1||row.size()>1))
        return std::nullopt;
    for(size_t i=0;i<=end;++i)first.kinds[i]=WordKind::Marked;
    return name;
}

std::vector<SortedPiece> SubtitleStyle::pickNamed(std::vector<SortedPiece> sorted,std::vector<std::pair<std::wstring,std::wstring>>& parts){
    parts.clear();
    std::vector<Piece> pieces;for(const auto& sp:sorted)pieces.push_back(sp.piece);
    std::vector<char> keep(sorted.size(),0);
    std::optional<std::pair<float,float>> last; // middle across and down of the last row followed
    for(const auto& line:pieceRows(pieces)){
        // What stands together on the row (a menu word far off at the same height is not of it).
        std::vector<std::vector<size_t>> runs;
        for(size_t i:line){
            const auto b=pieces[i].box();
            if(!runs.empty()&&b.left-pieces[runs.back().back()].box().right<=1.5*(b.bottom-b.top))runs.back().push_back(i);
            else runs.push_back({i});
        }
        for(const auto& run:runs){
            int l=pieces[run.front()].box().left,r=l,t=1<<30,b=-(1<<30);
            for(size_t i:run){const auto x=pieces[i].box();r=std::max(r,x.right);t=std::min(t,x.top);b=std::max(b,x.bottom);}
            const float across=float(l+r)/2.0f,down=float(t+b)/2.0f,tall=float(b-t);
            std::vector<SortedPiece*> row;for(size_t i:run)row.push_back(&sorted[i]);
            if(auto name=named(row,across,tall)){
                namedHeights_.push_back(tall);if(namedHeights_.size()>30)namedHeights_.pop_front();
                last=std::make_pair(across,down);
                std::vector<std::wstring> said;
                for(size_t n=0;n<row.size();++n){const auto s=spokenOf(row[n]->piece,row[n]->kinds);said.push_back(n?s.whole:s.rest);}
                for(size_t i:run)keep[i]=1;
                parts.push_back({*name,joinWords(said)});
                continue;
            }
            if(!last||parts.empty())continue;
            const float usual=medianOf(namedHeights_);
            const float step=(down-last->second)/(kRowStep*usual);
            if(step>=kStepLow&&step<=kStepHigh&&std::abs(across-last->first)<=0.9f*usual){
                last=std::make_pair(across,down);
                std::vector<std::wstring> said{parts.back().second};
                for(auto* sp:row)said.push_back(spokenOf(sp->piece,sp->kinds).whole);
                for(size_t i:run)keep[i]=1;
                parts.back().second=joinWords(said);
            }else if(step>kStepHigh)last.reset(); // further down than a step: not of this subtitle
        }
    }
    std::vector<SortedPiece> kept;
    for(size_t i=0;i<sorted.size();++i)if(keep[i])kept.push_back(std::move(sorted[i]));
    return kept;
}

SubtitleText SubtitleStyle::read(const Crop& crop,std::vector<Piece> pieces){
    auto kept=sort(crop,std::move(pieces));
    SubtitleText out;
    if(rules_==GameRules::Gta6){kept=pickNamed(std::move(kept),out.parts);out.byParts=true;}
    for(const auto& sp:kept){
        if(std::find(sp.kinds.begin(),sp.kinds.end(),WordKind::Body)==sp.kinds.end())continue;
        const auto b=sp.piece.box();
        if(b.left<=2||b.right>=int(crop.w)-2){out.clipped=true;break;}
    }
    auto text=linesAndName(kept);
    out.lines=std::move(text.lines);out.name=std::move(text.name);
    return out;
}

} // namespace veyra::engine::screentext
