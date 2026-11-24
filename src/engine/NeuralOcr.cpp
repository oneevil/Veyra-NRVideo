// PP-OCRv5 text finding and reading, after RapidOCR 3.10.0 (https://github.com/RapidAI/RapidOCR,
// Apache-2.0, which follows PaddleOCR, Apache-2.0): the image preparation, the DBNet
// post-processing (with the round offset of Clipper 6.4.2, Boost licence, for growing a
// region back), the line crops, the CTC decoding and the word places are written out here
// step by step, with the OpenCV operations they use (resize, minimum-area rectangle,
// perspective crop) done by hand to give the same numbers.
#include "NeuralOcr.h"

#include "../tts/Ort.h"

#include <algorithm>
#include <cwctype>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numeric>

namespace veyra::engine::screentext {

using ocrimage::Bgr;

// ---------------------------------------------------------------------------------------------
// Image operations (cv2 equivalents)
// ---------------------------------------------------------------------------------------------
namespace ocrimage {

Bgr fromRgba(const Crop& crop){
    Bgr out;
    out.w=int(crop.w); out.h=int(crop.h);
    const size_t n=size_t(out.w)*out.h;
    out.px.resize(n*3);
    if(crop.rgba.size()<n*4) { out.w=out.h=0; out.px.clear(); return out; }
    for(size_t i=0;i<n;++i){
        out.px[i*3]=crop.rgba[i*4+2];
        out.px[i*3+1]=crop.rgba[i*4+1];
        out.px[i*3+2]=crop.rgba[i*4];
    }
    return out;
}

namespace {
inline uint8_t clampByte(double v){ return uint8_t(std::clamp(std::nearbyint(v),0.0,255.0)); }

struct LinearTap { int s0,s1; double f; };
std::vector<LinearTap> linearTaps(int src,int dst){
    std::vector<LinearTap> taps(size_t(std::max(dst,0)));
    const double scale=double(src)/dst;
    for(int d=0;d<dst;++d){
        float fx=float((d+0.5)*scale-0.5);
        int sx=int(std::floor(fx));
        fx-=float(sx);
        if(sx<0){ fx=0; sx=0; }
        if(sx>=src-1){ fx=0; sx=src-1; }
        taps[size_t(d)]={sx,std::min(sx+1,src-1),double(fx)};
    }
    return taps;
}
} // namespace

// Measured against cv2 5.x: the horizontal pass is rounded to whole values, the vertical
// one uses 11-bit coefficients (the result then differs from cv2 in about 0.1% of the
// pixels, by one level).
Bgr resizeLinear(const Bgr& src,int w,int h){
    Bgr out; out.w=w; out.h=h;
    if(w<=0||h<=0||src.w<=0||src.h<=0) { out.w=out.h=0; return out; }
    out.px.resize(size_t(w)*h*3);
    const auto tx=linearTaps(src.w,w),ty=linearTaps(src.h,h);
    std::vector<int32_t> rows(size_t(src.h)*w*3);
    std::vector<char> done(size_t(src.h),0);
    auto horizontal=[&](int y){
        if(done[size_t(y)]) return;
        done[size_t(y)]=1;
        const uint8_t* s=src.px.data()+size_t(y)*src.w*3;
        int32_t* d=rows.data()+size_t(y)*w*3;
        for(int x=0;x<w;++x){
            const auto& t=tx[size_t(x)];
            const float f=float(t.f);
            const int a0=int(std::nearbyint((1.f-f)*2048.f)),a1=int(std::nearbyint(f*2048.f));
            for(int c=0;c<3;++c) d[x*3+c]=(s[t.s0*3+c]*a0+s[t.s1*3+c]*a1+1024)>>11;
        }
    };
    for(int y=0;y<h;++y){
        const auto& t=ty[size_t(y)];
        const float fy=float(t.f);
        const int b0=int(std::nearbyint((1.f-fy)*2048.f)),b1=int(std::nearbyint(fy*2048.f));
        horizontal(t.s0); horizontal(t.s1);
        const int32_t* r0=rows.data()+size_t(t.s0)*w*3;
        const int32_t* r1=rows.data()+size_t(t.s1)*w*3;
        uint8_t* d=out.px.data()+size_t(y)*w*3;
        for(int i=0;i<w*3;++i){
            const int v=(r0[i]*b0+r1[i]*b1+1024)>>11;
            d[i]=uint8_t(std::clamp(v,0,255));
        }
    }
    return out;
}

namespace {
struct AreaTap { int d,s; float a; };
std::vector<AreaTap> areaTaps(int ssize,int dsize,double scale){
    std::vector<AreaTap> tab;
    for(int dx=0;dx<dsize;++dx){
        const double f1=dx*scale,f2=f1+scale;
        const double cell=std::min(scale,ssize-f1);
        int s1=int(std::ceil(f1)),s2=int(std::floor(f2));
        s2=std::min(s2,ssize-1);
        s1=std::min(s1,s2);
        if(s1-f1>1e-3) tab.push_back({dx,s1-1,float((s1-f1)/cell)});
        for(int s=s1;s<s2;++s) tab.push_back({dx,s,float(1.0/cell)});
        if(f2-s2>1e-3) tab.push_back({dx,s2,float(std::min(std::min(f2-s2,1.0),cell)/cell)});
    }
    return tab;
}
} // namespace

Bgr resizeArea(const Bgr& src,double f){
    Bgr out;
    out.w=int(std::nearbyint(src.w*f)); out.h=int(std::nearbyint(src.h*f));
    if(out.w<=0||out.h<=0) { out.w=out.h=0; return out; }
    const auto tx=areaTaps(src.w,out.w,1.0/f),ty=areaTaps(src.h,out.h,1.0/f);
    // Across first (each source row once), then down, in float as OpenCV does.
    const size_t rowLen=size_t(out.w)*3;
    std::vector<float> rows(size_t(src.h)*rowLen,0.f);
    for(int y=0;y<src.h;++y){
        const uint8_t* s=src.px.data()+size_t(y)*src.w*3;
        float* row=rows.data()+size_t(y)*rowLen;
        for(const auto& t:tx) for(int c=0;c<3;++c) row[size_t(t.d)*3+size_t(c)]+=float(s[t.s*3+c])*t.a;
    }
    std::vector<float> acc(rowLen);
    out.px.resize(size_t(out.w)*out.h*3);
    size_t k=0;
    for(int dy=0;dy<out.h;++dy){
        std::fill(acc.begin(),acc.end(),0.f);
        for(;k<ty.size()&&ty[k].d==dy;++k){
            const float* row=rows.data()+size_t(ty[k].s)*rowLen;
            for(size_t i=0;i<rowLen;++i) acc[i]+=row[i]*ty[k].a;
        }
        uint8_t* d=out.px.data()+size_t(dy)*rowLen;
        for(size_t i=0;i<rowLen;++i) d[i]=clampByte(acc[i]);
    }
    return out;
}

} // namespace ocrimage

namespace {

// ---------------------------------------------------------------------------------------------
// Small geometry
// ---------------------------------------------------------------------------------------------
struct P2 { double x=0,y=0; };
using Quad=std::array<P2,4>;

double dist(const P2& a,const P2& b){ return std::hypot(a.x-b.x,a.y-b.y); }

// 3x3 homography taking src[i] to dst[i] (cv2.getPerspectiveTransform).
std::array<double,9> perspective(const Quad& src,const Quad& dst){
    double a[8][9]={};
    for(int i=0;i<4;++i){
        const double x=src[size_t(i)].x,y=src[size_t(i)].y,u=dst[size_t(i)].x,v=dst[size_t(i)].y;
        double* r0=a[i]; double* r1=a[i+4];
        r0[0]=x; r0[1]=y; r0[2]=1; r0[6]=-x*u; r0[7]=-y*u; r0[8]=u;
        r1[3]=x; r1[4]=y; r1[5]=1; r1[6]=-x*v; r1[7]=-y*v; r1[8]=v;
    }
    for(int c=0;c<8;++c){
        int p=c;
        for(int r=c+1;r<8;++r) if(std::abs(a[r][c])>std::abs(a[p][c])) p=r;
        if(p!=c) for(int k=0;k<9;++k) std::swap(a[c][k],a[p][k]);
        const double d=a[c][c];
        if(std::abs(d)<1e-300) return {1,0,0,0,1,0,0,0,1};
        for(int r=0;r<8;++r){
            if(r==c) continue;
            const double m=a[r][c]/d;
            if(m!=0) for(int k=c;k<9;++k) a[r][k]-=m*a[c][k];
        }
    }
    std::array<double,9> m{};
    for(int i=0;i<8;++i) m[size_t(i)]=a[i][8]/a[i][i];
    m[8]=1;
    return m;
}

std::array<double,9> invert3(const std::array<double,9>& m){
    const double a=m[0],b=m[1],c=m[2],d=m[3],e=m[4],f=m[5],g=m[6],h=m[7],i=m[8];
    const double A=e*i-f*h,B=-(d*i-f*g),C=d*h-e*g;
    const double det=a*A+b*B+c*C;
    if(std::abs(det)<1e-300) return {0,0,0,0,0,0,0,0,0};
    const double r=1.0/det;
    return {A*r,-(b*i-c*h)*r,(b*f-c*e)*r, B*r,(a*i-c*g)*r,-(a*f-c*d)*r, C*r,-(a*h-b*g)*r,(a*e-b*d)*r};
}

// Convex hull, counter-clockwise in the usual sense (monotone chain).
std::vector<P2> hull(std::vector<P2> pts){
    std::sort(pts.begin(),pts.end(),[](const P2& a,const P2& b){ return a.x<b.x||(a.x==b.x&&a.y<b.y); });
    pts.erase(std::unique(pts.begin(),pts.end(),[](const P2& a,const P2& b){ return a.x==b.x&&a.y==b.y; }),pts.end());
    if(pts.size()<3) return pts;
    auto cross=[](const P2& o,const P2& a,const P2& b){ return (a.x-o.x)*(b.y-o.y)-(a.y-o.y)*(b.x-o.x); };
    std::vector<P2> h(pts.size()*2);
    size_t k=0;
    for(const auto& p:pts){ while(k>=2&&cross(h[k-2],h[k-1],p)<=0) --k; h[k++]=p; }
    for(size_t i=pts.size()-1,t=k+1;i-->0;){ while(k>=t&&cross(h[k-2],h[k-1],pts[i])<=0) --k; h[k++]=pts[i]; }
    h.resize(k-1);
    return h;
}

// cv2.minAreaRect + cv2.boxPoints, in float as OpenCV computes them (the hull starts at its
// top point, rightmost of those, and runs clockwise on screen; the rotating calipers keep
// the last rectangle of least area), then the corner order of RapidOCR's get_mini_boxes
// (top left, top right, bottom right, bottom left). Returns the shorter side.
double miniBox(const std::vector<P2>& points,Quad& box){
    auto h=hull(points);
    float cx=0,cy=0,width=0,height=0,angle=0;
    const int n=int(h.size());
    if(n>2){
        size_t start=0;
        for(size_t i=1;i<h.size();++i)
            if(h[i].y<h[start].y||(h[i].y==h[start].y&&h[i].x>h[start].x)) start=i;
        std::rotate(h.begin(),h.begin()+std::ptrdiff_t(start),h.end());
        const size_t un=static_cast<size_t>(n);
        std::vector<float> px(un),py(un),vx(un),vy(un),inv(un);
        for(int i=0;i<n;++i){ px[size_t(i)]=float(h[size_t(i)].x); py[size_t(i)]=float(h[size_t(i)].y); }
        int left=0,bottom=0,right=0,top=0;
        float leftX=px[0],rightX=px[0],topY=py[0],bottomY=py[0];
        float p0x=px[0],p0y=py[0];
        for(int i=0;i<n;++i){
            if(p0x<leftX){ leftX=p0x; left=i; }
            if(p0x>rightX){ rightX=p0x; right=i; }
            if(p0y>topY){ topY=p0y; top=i; }
            if(p0y<bottomY){ bottomY=p0y; bottom=i; }
            const int j=i+1<n?i+1:0;
            const double dx=double(px[size_t(j)])-p0x,dy=double(py[size_t(j)])-p0y;
            vx[size_t(i)]=float(dx); vy[size_t(i)]=float(dy);
            inv[size_t(i)]=float(1.0/std::sqrt(dx*dx+dy*dy));
            p0x=px[size_t(j)]; p0y=py[size_t(j)];
        }
        float orientation=0;
        {
            double ax=vx[size_t(n-1)],ay=vy[size_t(n-1)];
            for(int i=0;i<n;++i){
                const double bx=vx[size_t(i)],by=vy[size_t(i)];
                const double convexity=ax*by-ay*bx;
                if(convexity!=0){ orientation=convexity>0?1.f:-1.f; break; }
                ax=bx; ay=by;
            }
        }
        if(orientation==0) orientation=1.f;
        float baseA=orientation,baseB=0;
        int seq[4]={bottom,right,top,left};
        float minArea=std::numeric_limits<float>::max();
        int bufLeft=0,bufBottom=0; float bufA=0,bufW=0,bufB=0,bufH=0;
        for(int k=0;k<n;++k){
            const float dp[4]={
                baseA*vx[size_t(seq[0])]+baseB*vy[size_t(seq[0])],
                -baseB*vx[size_t(seq[1])]+baseA*vy[size_t(seq[1])],
                -baseA*vx[size_t(seq[2])]-baseB*vy[size_t(seq[2])],
                baseB*vx[size_t(seq[3])]-baseA*vy[size_t(seq[3])],
            };
            int mainElement=0;
            float maxCos=dp[0]*inv[size_t(seq[0])];
            for(int i=1;i<4;++i){
                const float c=dp[i]*inv[size_t(seq[i])];
                if(c>maxCos){ mainElement=i; maxCos=c; }
            }
            const int pi=seq[mainElement];
            const float leadX=vx[size_t(pi)]*inv[size_t(pi)],leadY=vy[size_t(pi)]*inv[size_t(pi)];
            switch(mainElement){
            case 0: baseA=leadX; baseB=leadY; break;
            case 1: baseA=leadY; baseB=-leadX; break;
            case 2: baseA=-leadX; baseB=-leadY; break;
            default: baseA=-leadY; baseB=leadX; break;
            }
            seq[mainElement]=seq[mainElement]+1==n?0:seq[mainElement]+1;
            float dx=px[size_t(seq[1])]-px[size_t(seq[3])],dy=py[size_t(seq[1])]-py[size_t(seq[3])];
            const float w=dx*baseA+dy*baseB;
            dx=px[size_t(seq[2])]-px[size_t(seq[0])]; dy=py[size_t(seq[2])]-py[size_t(seq[0])];
            const float hgt=-dx*baseB+dy*baseA;
            const float area=w*hgt;
            if(area<=minArea){
                minArea=area; bufLeft=seq[3]; bufA=baseA; bufW=w; bufB=baseB; bufH=hgt; bufBottom=seq[0];
            }
        }
        const float A1=bufA,B1=bufB,A2=-bufB,B2=bufA;
        const float C1=A1*px[size_t(bufLeft)]+py[size_t(bufLeft)]*B1;
        const float C2=A2*px[size_t(bufBottom)]+py[size_t(bufBottom)]*B2;
        const float idet=1.f/(A1*B2-A2*B1);
        const float o0x=(C1*B2-C2*B1)*idet,o0y=(A1*C2-A2*C1)*idet;
        const float o1x=A1*bufW,o1y=B1*bufW,o2x=A2*bufH,o2y=B2*bufH;
        cx=o0x+(o1x+o2x)*0.5f; cy=o0y+(o1y+o2y)*0.5f;
        width=float(std::sqrt(double(o1x)*o1x+double(o1y)*o1y));
        height=float(std::sqrt(double(o2x)*o2x+double(o2y)*o2y));
        angle=float(std::atan2(double(o1y),double(o1x)));
    } else if(n==2){
        cx=float((h[0].x+h[1].x)*0.5); cy=float((h[0].y+h[1].y)*0.5);
        const double dx=h[1].x-h[0].x,dy=h[1].y-h[0].y;
        width=float(std::sqrt(dx*dx+dy*dy)); height=0;
        angle=float(std::atan2(dy,dx));
    } else if(n==1){
        cx=float(h[0].x); cy=float(h[0].y);
    }
    angle=float(angle*180/3.141592653589793);
    // RotatedRect::points
    const double rad=angle*3.141592653589793/180.;
    const float b=float(std::cos(rad))*0.5f,a=float(std::sin(rad))*0.5f;
    std::array<P2,4> pts;
    float q0x=cx-a*height-b*width,q0y=cy+b*height-a*width;
    float q1x=cx+a*height-b*width,q1y=cy-b*height-a*width;
    pts[0]={q0x,q0y}; pts[1]={q1x,q1y};
    pts[2]={2*cx-q0x,2*cy-q0y}; pts[3]={2*cx-q1x,2*cy-q1y};
    std::stable_sort(pts.begin(),pts.end(),[](const P2& p,const P2& q){ return p.x<q.x; });
    int i1,i2,i3,i4;
    if(pts[1].y>pts[0].y){ i1=0; i4=1; } else { i1=1; i4=0; }
    if(pts[3].y>pts[2].y){ i2=2; i3=3; } else { i2=3; i3=2; }
    box={pts[size_t(i1)],pts[size_t(i2)],pts[size_t(i3)],pts[size_t(i4)]};
    return double(std::min(width,height));
}

// Clipper 6.4.2 ClipperOffset with round joins, one closed polygon (pyclipper truncates
// the coordinates to integers first). Returns the grown polygon's integer points.
std::vector<P2> roundOffset(const Quad& box,double delta){
    std::vector<std::array<long long,2>> src;
    for(const auto& p:box){
        std::array<long long,2> q{(long long)p.x,(long long)p.y};
        if(src.empty()||src.back()!=q) src.push_back(q);
    }
    while(src.size()>1&&src.front()==src.back()) src.pop_back();
    std::vector<P2> out;
    if(src.size()<3) return out;
    double area=0;
    for(size_t i=0,j=src.size()-1;i<src.size();j=i++)
        area+=(double(src[j][0])+double(src[i][0]))*(double(src[j][1])-double(src[i][1]));
    if(-area*0.5<0) std::reverse(src.begin(),src.end());
    auto round=[](double v){ return v<0?(long long)(v-0.5):(long long)(v+0.5); };
    const double pi=3.141592653589793238,twoPi=pi*2;
    const double tol=0.25;
    const double y=tol>std::abs(delta)*0.25?std::abs(delta)*0.25:tol;
    double steps=pi/std::acos(1-y/std::abs(delta));
    if(steps>std::abs(delta)*pi) steps=std::abs(delta)*pi;
    double sn=std::sin(twoPi/steps);
    const double cs=std::cos(twoPi/steps);
    const double perRad=steps/twoPi;
    if(delta<0) sn=-sn;
    const size_t n=src.size();
    std::vector<P2> normals(n);
    for(size_t j=0;j<n;++j){
        const auto& a=src[j]; const auto& b=src[(j+1)%n];
        const double dx=double(b[0]-a[0]),dy=double(b[1]-a[1]);
        if(dx==0&&dy==0){ normals[j]={0,0}; continue; }
        const double f=1.0/std::sqrt(dx*dx+dy*dy);
        normals[j]={dy*f,-dx*f};
    }
    auto push=[&](long long x,long long yy){ out.push_back(P2{double(x),double(yy)}); };
    size_t k=n-1;
    for(size_t j=0;j<n;++j){
        const double sx=double(src[j][0]),sy=double(src[j][1]);
        double sinA=normals[k].x*normals[j].y-normals[j].x*normals[k].y;
        if(std::abs(sinA*delta)<1.0){
            const double cosA=normals[k].x*normals[j].x+normals[j].y*normals[k].y;
            // (Clipper returns here without moving k on.)
            if(cosA>0){ push(round(sx+normals[k].x*delta),round(sy+normals[k].y*delta)); continue; }
        } else if(sinA>1.0) sinA=1.0; else if(sinA<-1.0) sinA=-1.0;
        {
            if(sinA*delta<0){
                push(round(sx+normals[k].x*delta),round(sy+normals[k].y*delta));
                push(src[j][0],src[j][1]);
                push(round(sx+normals[j].x*delta),round(sy+normals[j].y*delta));
            } else {
                const double a=std::atan2(sinA,normals[k].x*normals[j].x+normals[k].y*normals[j].y);
                const int st=std::max(int(round(perRad*std::abs(a))),1);
                double X=normals[k].x,Y=normals[k].y;
                for(int i=0;i<st;++i){
                    push(round(sx+X*delta),round(sy+Y*delta));
                    const double X2=X;
                    X=X*cs-sn*Y;
                    Y=X2*sn+Y*cs;
                }
                push(round(sx+normals[j].x*delta),round(sy+normals[j].y*delta));
            }
        }
        k=j;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// DBNet post-processing
// ---------------------------------------------------------------------------------------------
struct DetBox { Quad q; double score=0; };

// Mean of the map inside the box (cv2.fillPoly of the truncated corners, edges included).
double boxScore(const std::vector<float>& pred,int w,int h,const Quad& box){
    double x0=1e300,x1=-1e300,y0=1e300,y1=-1e300;
    for(const auto& p:box){ x0=std::min(x0,p.x); x1=std::max(x1,p.x); y0=std::min(y0,p.y); y1=std::max(y1,p.y); }
    const int xmin=std::clamp(int(std::floor(x0)),0,w-1),xmax=std::clamp(int(std::ceil(x1)),0,w-1);
    const int ymin=std::clamp(int(std::floor(y0)),0,h-1),ymax=std::clamp(int(std::ceil(y1)),0,h-1);
    std::array<P2,4> v;
    for(size_t i=0;i<4;++i) v[i]={double((long long)(float(box[i].x-xmin))),double((long long)(float(box[i].y-ymin)))};
    double py0=1e300,py1=-1e300;
    for(const auto& p:v){ py0=std::min(py0,p.y); py1=std::max(py1,p.y); }
    double sum=0; size_t count=0;
    const int mw=xmax-xmin+1,mh=ymax-ymin+1;
    for(int yy=std::max(0,int(py0));yy<=std::min(mh-1,int(py1));++yy){
        // The polygon's extent over the row's band [y-0.5, y+0.5].
        const double b0=std::max(py0,yy-0.5),b1=std::min(py1,yy+0.5);
        double lo=1e300,hi=-1e300;
        for(size_t i=0;i<4;++i){
            const P2& a=v[i]; const P2& b=v[(i+1)%4];
            const double ey0=std::min(a.y,b.y),ey1=std::max(a.y,b.y);
            const double c0=std::max(ey0,b0),c1=std::min(ey1,b1);
            if(c0>c1) continue;
            for(double yc:{c0,c1}){
                const double x=a.y==b.y?(yc==a.y?std::min(a.x,b.x):a.x):a.x+(b.x-a.x)*(yc-a.y)/(b.y-a.y);
                const double x2=a.y==b.y?std::max(a.x,b.x):x;
                lo=std::min(lo,std::min(x,x2)); hi=std::max(hi,std::max(x,x2));
            }
        }
        if(lo>hi) continue;
        const int l=std::max(0,int(std::floor(lo+0.5))),r=std::min(mw-1,int(std::ceil(hi-0.5)));
        const float* row=pred.data()+size_t(ymin+yy)*w+xmin;
        for(int xx=l;xx<=r;++xx){ sum+=row[xx]; ++count; }
    }
    return count?sum/double(count):0.0;
}

std::vector<DetBox> boxesFromMap(const std::vector<float>& pred,int w,int h,int destW,int destH,
                                 const NeuralOcrSettings& s){
    // Pixels above the threshold, grown by a 2x2 square (cv2.dilate, anchor at its centre).
    std::vector<uint8_t> seg(size_t(w)*h),mask(size_t(w)*h);
    for(size_t i=0;i<seg.size();++i) seg[i]=pred[i]>s.pixelThresh;
    for(int y=0;y<h;++y) for(int x=0;x<w;++x){
        uint8_t v=seg[size_t(y)*w+x];
        if(x>0) v|=seg[size_t(y)*w+x-1];
        if(y>0){ v|=seg[size_t(y-1)*w+x]; if(x>0) v|=seg[size_t(y-1)*w+x-1]; }
        mask[size_t(y)*w+x]=v;
    }
    // Contours (cv2.findContours RETR_LIST): the outer border of each 8-connected region and
    // the border around each hole (a 4-connected background region off the edges). Only
    // their convex hulls matter here. Labelled by runs along the rows.
    struct Run { int y,x0,x1; bool fg; int parent; };
    std::vector<Run> runs;
    std::vector<size_t> rowStart(size_t(h)+1,0);
    for(int y=0;y<h;++y){
        rowStart[size_t(y)]=runs.size();
        const uint8_t* m=mask.data()+size_t(y)*w;
        for(int x=0;x<w;){
            int e=x;
            while(e+1<w&&m[e+1]==m[x]) ++e;
            runs.push_back({y,x,e,m[x]!=0,int(runs.size())});
            x=e+1;
        }
    }
    rowStart[size_t(h)]=runs.size();
    auto find=[&](int i){ while(runs[size_t(i)].parent!=i){ runs[size_t(i)].parent=runs[size_t(runs[size_t(i)].parent)].parent; i=runs[size_t(i)].parent; } return i; };
    for(int y=1;y<h;++y){
        size_t i=rowStart[size_t(y-1)],j=rowStart[size_t(y)];
        const size_t ie=rowStart[size_t(y)],je=rowStart[size_t(y+1)];
        // Every pair of runs of the same kind that touch (diagonally too, for text pixels).
        for(size_t a=i;a<ie;++a){
            const Run& ra=runs[a];
            while(j<je&&runs[j].x1<ra.x0-1) ++j;
            for(size_t b=j;b<je&&runs[b].x0<=ra.x1+1;++b){
                const Run& rb=runs[b];
                if(ra.fg!=rb.fg) continue;
                if(!ra.fg&&(rb.x1<ra.x0||rb.x0>ra.x1)) continue;
                const int pa=find(int(a)),pb=find(int(b));
                if(pa!=pb) runs[size_t(std::max(pa,pb))].parent=std::min(pa,pb);
            }
        }
    }
    std::vector<int> slot(runs.size(),-1);
    std::vector<std::vector<P2>> contours;
    std::vector<char> edge;
    for(size_t r=0;r<runs.size();++r){
        const int root=find(int(r));
        int& k=slot[size_t(root)];
        if(k<0){ k=int(contours.size()); contours.emplace_back(); edge.push_back(0); }
        const Run& run=runs[r];
        auto& pts=contours[size_t(k)];
        if(run.fg){
            pts.push_back({double(run.x0),double(run.y)}); pts.push_back({double(run.x1),double(run.y)});
        } else {
            if(run.x0==0||run.x1==w-1||run.y==0||run.y==h-1) edge[size_t(k)]=1;
            const double y=run.y;
            pts.push_back({double(run.x0-1),y}); pts.push_back({double(run.x1+1),y});
            pts.push_back({double(run.x0),y-1}); pts.push_back({double(run.x1),y-1});
            pts.push_back({double(run.x0),y+1}); pts.push_back({double(run.x1),y+1});
        }
    }
    for(size_t k=0;k<contours.size();++k) if(edge[k]) contours[k].clear();
    std::vector<DetBox> boxes;
    const int minSize=3;
    size_t considered=0;
    for(const auto& contour:contours){
        if(contour.empty()) continue;
        if(++considered>1000) break;
        Quad box;
        if(miniBox(contour,box)<minSize) continue;
        const double score=boxScore(pred,w,h,box);
        if(s.boxThresh>score) continue;
        // Grow back by area * ratio / perimeter.
        double area=0,length=0;
        for(size_t i=0;i<4;++i){
            const P2& a=box[i]; const P2& b=box[(i+1)%4];
            area+=a.x*b.y-b.x*a.y; length+=dist(a,b);
        }
        area=std::abs(area)*0.5;
        const auto grown=roundOffset(box,length>0?area*s.unclipRatio/length:0);
        if(grown.empty()) continue;
        Quad g;
        if(miniBox(grown,g)<minSize+2) continue;
        DetBox d; d.score=score;
        for(size_t i=0;i<4;++i){
            const float fx=float(g[i].x)/float(w)*float(destW),fy=float(g[i].y)/float(h)*float(destH);
            d.q[i]={std::clamp(double(std::nearbyint(fx)),0.0,double(destW)),std::clamp(double(std::nearbyint(fy)),0.0,double(destH))};
        }
        boxes.push_back(d);
    }
    // filter_det_res: corners clockwise from the top left, kept inside, at least 4 px each way.
    std::vector<DetBox> kept;
    for(auto& d:boxes){
        std::array<P2,4> p=d.q;
        std::stable_sort(p.begin(),p.end(),[](const P2& a,const P2& b){ return a.x<b.x; });
        std::array<P2,2> left{p[0],p[1]},right{p[2],p[3]};
        std::stable_sort(left.begin(),left.end(),[](const P2& a,const P2& b){ return a.y<b.y; });
        std::stable_sort(right.begin(),right.end(),[](const P2& a,const P2& b){ return a.y<b.y; });
        Quad q{left[0],right[0],right[1],left[1]};
        for(auto& c:q){
            c.x=double(int(std::min(std::max(c.x,0.0),double(destW-1))));
            c.y=double(int(std::min(std::max(c.y,0.0),double(destH-1))));
        }
        if(int(dist(q[0],q[1]))<=3||int(dist(q[0],q[3]))<=3) continue;
        kept.push_back({q,d.score});
    }
    // Top to bottom, then left to right within a line (boxes whose tops are under 10 px apart).
    std::stable_sort(kept.begin(),kept.end(),[](const DetBox& a,const DetBox& b){ return a.q[0].y<b.q[0].y; });
    std::vector<int> line(kept.size(),0);
    for(size_t i=1;i<kept.size();++i) line[i]=line[i-1]+(kept[i].q[0].y-kept[i-1].q[0].y>=10?1:0);
    std::vector<size_t> order(kept.size());
    std::iota(order.begin(),order.end(),size_t(0));
    std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b){
        return line[a]!=line[b]?line[a]<line[b]:kept[a].q[0].x<kept[b].q[0].x;
    });
    std::vector<DetBox> sorted;
    for(size_t i:order) sorted.push_back(kept[i]);
    return sorted;
}

// ---------------------------------------------------------------------------------------------
// Line crops (get_rotate_crop_image: perspective warp, bicubic, edges repeated)
// ---------------------------------------------------------------------------------------------
struct CubicTable {
    std::array<std::array<int16_t,16>,32*32> tab{};
    CubicTable(){
        auto coeffs=[](float x,float* c){
            const float A=-0.75f;
            c[0]=((A*(x+1)-5*A)*(x+1)+8*A)*(x+1)-4*A;
            c[1]=((A+2)*x-(A+3))*x*x+1;
            c[2]=((A+2)*(1-x)-(A+3))*(1-x)*(1-x)+1;
            c[3]=1.f-c[0]-c[1]-c[2];
        };
        for(int fy=0;fy<32;++fy) for(int fx=0;fx<32;++fx){
            float cy[4],cx[4];
            coeffs(fy/32.f,cy); coeffs(fx/32.f,cx);
            auto& t=tab[size_t(fy*32+fx)];
            int sum=0;
            for(int i=0;i<4;++i) for(int j=0;j<4;++j){
                const int v=std::clamp(int(std::nearbyint(cy[i]*cx[j]*32768.f)),-32768,32767);
                t[size_t(i*4+j)]=int16_t(v); sum+=v;
            }
            if(sum!=32768){
                const int diff=sum-32768;
                int mk1=2,mk2=2,Mk1=2,Mk2=2;
                for(int k1=2;k1<4;++k1) for(int k2=2;k2<4;++k2){
                    if(t[size_t(k1*4+k2)]<t[size_t(mk1*4+mk2)]){ mk1=k1; mk2=k2; }
                    else if(t[size_t(k1*4+k2)]>t[size_t(Mk1*4+Mk2)]){ Mk1=k1; Mk2=k2; }
                }
                if(diff<0) t[size_t(Mk1*4+Mk2)]=int16_t(t[size_t(Mk1*4+Mk2)]-diff);
                else t[size_t(mk1*4+mk2)]=int16_t(t[size_t(mk1*4+mk2)]-diff);
            }
        }
    }
};

Bgr cropQuad(const Bgr& img,const Quad& q){
    static const CubicTable cubic;
    const int cw=int(std::max(dist(q[0],q[1]),dist(q[2],q[3])));
    const int ch=int(std::max(dist(q[0],q[3]),dist(q[1],q[2])));
    Bgr out; out.w=cw; out.h=ch;
    if(cw<=0||ch<=0) { out.w=out.h=0; return out; }
    out.px.resize(size_t(cw)*ch*3);
    Quad fq,dst{P2{0,0},P2{double(cw),0},P2{double(cw),double(ch)},P2{0,double(ch)}};
    for(size_t i=0;i<4;++i) fq[i]={double(float(q[i].x)),double(float(q[i].y))};
    const auto M=invert3(perspective(fq,dst));
    for(int y=0;y<ch;++y){
        for(int x=0;x<cw;++x){
            double W=M[6]*x+M[7]*y+M[8];
            W=W!=0?32.0/W:0;
            const double fX=std::clamp((M[0]*x+M[1]*y+M[2])*W,double(INT32_MIN),double(INT32_MAX));
            const double fY=std::clamp((M[3]*x+M[4]*y+M[5])*W,double(INT32_MIN),double(INT32_MAX));
            const int X=int(std::nearbyint(fX)),Y=int(std::nearbyint(fY));
            const int sx=(X>>5)-1,sy=(Y>>5)-1;
            const auto& t=cubic.tab[size_t((Y&31)*32+(X&31))];
            int acc[3]={0,0,0};
            for(int i=0;i<4;++i){
                const int yy=std::clamp(sy+i,0,img.h-1);
                const uint8_t* row=img.px.data()+size_t(yy)*img.w*3;
                for(int j=0;j<4;++j){
                    const int xx=std::clamp(sx+j,0,img.w-1);
                    const int c=t[size_t(i*4+j)];
                    acc[0]+=row[xx*3]*c; acc[1]+=row[xx*3+1]*c; acc[2]+=row[xx*3+2]*c;
                }
            }
            uint8_t* d=out.px.data()+(size_t(y)*cw+x)*3;
            for(int c=0;c<3;++c) d[c]=uint8_t(std::clamp((acc[c]+(1<<14))>>15,0,255));
        }
    }
    if(double(ch)/double(cw)>=1.5){ // np.rot90: a quarter turn anticlockwise
        Bgr r; r.w=out.h; r.h=out.w; r.px.resize(out.px.size());
        for(int i=0;i<r.h;++i) for(int j=0;j<r.w;++j)
            std::memcpy(&r.px[(size_t(i)*r.w+j)*3],&out.px[(size_t(j)*out.w+(out.w-1-i))*3],3);
        return r;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------------------------
std::vector<uint32_t> utf8Codes(const std::string& s){
    std::vector<uint32_t> out;
    for(size_t i=0;i<s.size();){
        const unsigned char c=static_cast<unsigned char>(s[i]);
        uint32_t cp=0; int n=0;
        if(c<0x80){ cp=c; n=1; }
        else if((c>>5)==6){ cp=c&0x1F; n=2; }
        else if((c>>4)==14){ cp=c&0x0F; n=3; }
        else if((c>>3)==30){ cp=c&0x07; n=4; }
        else { ++i; out.push_back(0xFFFD); continue; }
        if(i+size_t(n)>s.size()){ out.push_back(0xFFFD); break; }
        for(int k=1;k<n;++k) cp=(cp<<6)|(static_cast<unsigned char>(s[i+size_t(k)])&0x3F);
        out.push_back(cp); i+=size_t(n);
    }
    return out;
}

void appendCode(std::wstring& out,uint32_t cp){
    if constexpr(sizeof(wchar_t)==2){
        if(cp>=0x10000){
            cp-=0x10000;
            out.push_back(wchar_t(0xD800+(cp>>10)));
            out.push_back(wchar_t(0xDC00+(cp&0x3FF)));
            return;
        }
    }
    out.push_back(wchar_t(cp));
}

bool isChinese(uint32_t c){
    return (c>=0x4E00&&c<=0x9FFF)||(c>=0x3000&&c<=0x303F)||(c>=0xFF00&&c<=0xFFEF);
}
bool isSpace(uint32_t c){ // Python str.isspace
    return c==' '||(c>=0x09&&c<=0x0D)||(c>=0x1C&&c<=0x1F)||c==0x85||c==0xA0||c==0x1680||
           (c>=0x2000&&c<=0x200A)||c==0x2028||c==0x2029||c==0x202F||c==0x205F||c==0x3000;
}

double round5(double v){ return std::nearbyint(v*1e5)/1e5; }

struct LineRead {
    std::vector<uint32_t> codes;
    double score=0;
    // Word grouping (get_word_info): per word its code points and CTC columns.
    std::vector<std::vector<uint32_t>> words;
    std::vector<std::vector<int>> cols;
    std::vector<bool> chinese;
    double lineLen=0; // line_txt_len
};

} // namespace

// ---------------------------------------------------------------------------------------------
// NeuralOcr
// ---------------------------------------------------------------------------------------------
struct NeuralOcr::Look {
    std::vector<Quad> boxes;   // in the looked-at picture's pixels
    std::vector<std::wstring> texts;
    std::vector<double> scores;
    std::vector<std::vector<PieceWord>> words;
};

NeuralOcr::NeuralOcr(const tts::OrtRuntime& runtime,const std::filesystem::path& detector,
                     const std::filesystem::path& reader,NeuralOcrSettings settings):settings_(settings){
    detector_=std::make_unique<tts::OrtModel>(runtime,detector,settings.threads,false);
    reader_=std::make_unique<tts::OrtModel>(runtime,reader,settings.threads,false);
    auto first=[](const std::vector<std::string>& v,const char* what){
        if(v.empty()) throw tts::OrtError(std::string("model has no ")+what);
        return v.front();
    };
    detInput_=first(detector_->inputNames(),"input");
    detOutput_=first(detector_->outputNames(),"output");
    recInput_=first(reader_->inputNames(),"input");
    recOutput_=first(reader_->outputNames(),"output");
    const std::string chars=reader_->metadata("character");
    if(chars.empty()) throw tts::OrtError("the reader model carries no alphabet (metadata \"character\")");
    // splitlines(): one entry per line, the trailing line break makes no entry.
    alphabet_.push_back(L"");
    alphabetCodes_.push_back(0);
    size_t at=0;
    while(at<chars.size()){
        size_t end=chars.find('\n',at);
        if(end==std::string::npos) end=chars.size();
        std::string line=chars.substr(at,end-at);
        if(!line.empty()&&line.back()=='\r') line.pop_back();
        const auto codes=utf8Codes(line);
        std::wstring w;
        for(uint32_t c:codes) appendCode(w,c);
        alphabet_.push_back(w);
        alphabetCodes_.push_back(codes.empty()?0:codes.front());
        at=end+1;
    }
    alphabet_.push_back(L" ");
    alphabetCodes_.push_back(' ');
}

NeuralOcr::~NeuralOcr()=default;

NeuralOcr::Look NeuralOcr::look(const Crop& crop,double scale,bool recognise) const {
    Look result;
    Bgr picture=ocrimage::fromRgba(crop);
    if(picture.w<=0||picture.h<=0) return result;
    if(scale<1.0){
        picture=ocrimage::resizeArea(picture,scale);
        if(picture.w<=0||picture.h<=0) return result;
    }
    const int oriW=picture.w,oriH=picture.h;

    // Kept within 30..2000 px a side (resize_image_within_bounds), sizes in steps of 32.
    double ratioW=1,ratioH=1;
    Bgr img=picture;
    auto fit=[&](double ratio){
        int rh=int(std::nearbyint(int(img.h*ratio)/32.0))*32,rw=int(std::nearbyint(int(img.w*ratio)/32.0))*32;
        if(rw<=0||rh<=0) return false;
        const int h0=img.h,w0=img.w;
        img=ocrimage::resizeLinear(img,rw,rh);
        ratioH=double(h0)/rh; ratioW=double(w0)/rw;
        return true;
    };
    if(std::max(img.w,img.h)>2000){
        if(!fit(img.h>img.w?2000.0/img.h:2000.0/img.w)) return result;
    }
    if(std::min(img.w,img.h)<30){
        if(!fit(img.h<img.w?30.0/img.h:30.0/img.w)) return result;
    }
    // A wide or low picture gets black bands above and below (apply_vertical_padding).
    int padTop=0;
    if(img.h<=30||double(img.w)/img.h>8.0){
        const int newH=std::max(int(img.w/8.0),30)*2;
        padTop=int(std::abs(newH-img.h)/2);
        Bgr padded; padded.w=img.w; padded.h=img.h+2*padTop;
        padded.px.assign(size_t(padded.w)*padded.h*3,0);
        std::memcpy(padded.px.data()+size_t(padTop)*img.w*3,img.px.data(),img.px.size());
        img=std::move(padded);
    }

    // The finder: largest side at most limitSideLen, sides in steps of 32, BGR scaled to -1..1.
    double ratio=1.0;
    if(std::max(img.w,img.h)>settings_.limitSideLen)
        ratio=img.h>img.w?double(settings_.limitSideLen)/img.h:double(settings_.limitSideLen)/img.w;
    const int dh=int(std::nearbyint(int(img.h*ratio)/32.0))*32,dw=int(std::nearbyint(int(img.w*ratio)/32.0))*32;
    if(dw<=0||dh<=0) return result;
    const Bgr det=ocrimage::resizeLinear(img,dw,dh);
    std::vector<float> input(size_t(3)*dw*dh);
    const size_t plane=size_t(dw)*dh;
    for(size_t i=0;i<plane;++i) for(int c=0;c<3;++c)
        input[size_t(c)*plane+i]=(float(det.px[i*3+size_t(c)])*(1.f/255.f)-0.5f)/0.5f;
    const tts::Tensor x=tts::Tensor::of({1,3,dh,dw},std::move(input));
    const tts::Tensor map=detector_->run({{detInput_.c_str(),&x}},detOutput_.c_str());
    if(map.shape.size()!=4||map.shape[2]!=dh||map.shape[3]!=dw) throw tts::OrtError("unexpected text finder output");
    std::vector<float> pred(map.floats.begin(),map.floats.begin()+std::ptrdiff_t(plane));
    const auto found=boxesFromMap(pred,dw,dh,img.w,img.h,settings_);
    if(found.empty()) return result;

    // Places back in the picture's pixels (map_boxes_to_original).
    auto toPicture=[&](const Quad& q){
        Quad o;
        for(size_t i=0;i<4;++i){
            double px=double(float(q[i].x)*float(ratioW)),py=double((float(q[i].y)-float(padTop))*float(ratioH));
            px=std::min(std::max(px,0.0),double(oriW)); py=std::min(std::max(py,0.0),double(oriH));
            o[i]={px,py};
        }
        return o;
    };
    if(!recognise){
        for(const auto& d:found) result.boxes.push_back(toPicture(d.q));
        return result;
    }

    // The reader: crops sorted by aspect, batches of 6 padded to the widest, height 48.
    std::vector<Bgr> crops;
    for(const auto& d:found) crops.push_back(cropQuad(img,d.q));
    const size_t n=crops.size();
    std::vector<size_t> idx(n);
    std::iota(idx.begin(),idx.end(),size_t(0));
    auto aspect=[&](size_t i){ return crops[i].h>0?double(crops[i].w)/double(crops[i].h):0.0; };
    std::stable_sort(idx.begin(),idx.end(),[&](size_t a,size_t b){ return aspect(a)<aspect(b); });
    std::vector<LineRead> reads(n);
    for(size_t beg=0;beg<n;beg+=6){
        const size_t end=std::min(n,beg+6);
        double maxRatio=320.0/48.0;
        std::vector<double> ratios;
        for(size_t i=beg;i<end;++i){ const double r=aspect(idx[i]); maxRatio=std::max(maxRatio,r); ratios.push_back(r); }
        const int imgW=int(48*maxRatio);
        const size_t count=end-beg;
        std::vector<float> batch(count*3*48*size_t(imgW),0.f);
        for(size_t b=0;b<count;++b){
            const Bgr& c=crops[idx[beg+b]];
            if(c.w<=0||c.h<=0) continue;
            const double r=double(c.w)/double(c.h);
            const int rw=std::ceil(48*r)>imgW?imgW:int(std::ceil(48*r));
            const Bgr s=ocrimage::resizeLinear(c,rw,48);
            float* dst=batch.data()+b*3*48*size_t(imgW);
            for(int yy=0;yy<48;++yy) for(int xx=0;xx<rw;++xx) for(int ch=0;ch<3;++ch)
                dst[(size_t(ch)*48+yy)*imgW+xx]=(float(s.px[(size_t(yy)*rw+xx)*3+size_t(ch)])/255.f-0.5f)/0.5f;
        }
        const tts::Tensor bx=tts::Tensor::of({int64_t(count),3,48,imgW},std::move(batch));
        const tts::Tensor out=reader_->run({{recInput_.c_str(),&bx}},recOutput_.c_str());
        if(out.shape.size()!=3||size_t(out.shape[0])!=count) throw tts::OrtError("unexpected reader output");
        const int T=int(out.shape[1]),C=int(out.shape[2]);
        for(size_t b=0;b<count;++b){
            LineRead& lr=reads[idx[beg+b]];
            std::vector<int> tokens(static_cast<size_t>(T));
            std::vector<float> probs(static_cast<size_t>(T));
            for(int t=0;t<T;++t){
                const float* p=out.floats.data()+(b*size_t(T)+size_t(t))*size_t(C);
                int best=0;
                for(int k=1;k<C;++k) if(p[k]>p[best]) best=k;
                tokens[size_t(t)]=best; probs[size_t(t)]=p[best];
            }
            std::vector<int> cols;
            double sum=0; size_t m=0;
            for(int t=0;t<T;++t){
                const int tok=tokens[size_t(t)];
                if(tok==0||(t>0&&tok==tokens[size_t(t-1)])) continue;
                if(size_t(tok)>=alphabetCodes_.size()) continue;
                cols.push_back(t);
                lr.codes.push_back(alphabetCodes_[size_t(tok)]);
                sum+=round5(double(probs[size_t(t)])); ++m;
            }
            lr.score=m?round5(sum/double(m)):0.0;
            lr.lineLen=double(T)*ratios[b]/maxRatio;
            // Words: split at spaces, at a change between CJK and other letters, and where
            // the reader skipped more than 5 columns.
            if(cols.empty()) continue;
            std::vector<int> width(cols.size());
            for(size_t i=1;i<cols.size();++i) width[i]=cols[i]-cols[i-1];
            width[0]=std::min(isChinese(lr.codes[0])?3:2,cols[0]);
            int state=-1;
            std::vector<uint32_t> word; std::vector<int> wc;
            auto flush=[&]{ if(!word.empty()){ lr.words.push_back(word); lr.cols.push_back(wc); lr.chinese.push_back(state==1); word.clear(); wc.clear(); } };
            for(size_t i=0;i<lr.codes.size();++i){
                const uint32_t ch=lr.codes[i];
                if(isSpace(ch)){ flush(); continue; }
                const int cs=isChinese(ch)?1:0;
                if(state<0) state=cs;
                if(state!=cs||width[i]>5){ flush(); state=cs; }
                word.push_back(ch); wc.push_back(cols[i]);
            }
            flush();
        }
    }

    for(size_t i=0;i<n;++i){
        const LineRead& lr=reads[i];
        bool blank=true;
        for(uint32_t c:lr.codes) if(!isSpace(c)) { blank=false; break; }
        if(blank) continue;
        if(lr.score<settings_.textScore) continue;
        const Quad box=toPicture(found[i].q);
        std::wstring text;
        for(uint32_t c:lr.codes) appendCode(text,c);
        // Word places (cal_rec_boxes), in the crop as it would be at the picture's size.
        std::vector<PieceWord> words;
        const int cw=int(std::nearbyint(crops[i].w*ratioW)),ch=int(std::nearbyint(crops[i].h*ratioH));
        if(lr.lineLen>0&&cw>0&&ch>0&&!lr.words.empty()){
            const double colW=double(cw)/lr.lineLen;
            const bool allLatin=std::none_of(lr.chinese.begin(),lr.chinese.end(),[](bool b){ return b; });
            std::vector<double> charWidths;
            for(const auto& c:lr.cols)
                if(c.size()>1) charWidths.push_back((c.back()-c.front())*colW/double(c.size()-1));
            const double charW=!charWidths.empty()?std::accumulate(charWidths.begin(),charWidths.end(),0.0)/double(charWidths.size())
                                                   :double(cw)/double(lr.codes.size());
            auto cellLeft=[&](int col){ return double(std::max(int(((col+0.5)*colW)-charW/2),0)); };
            auto cellRight=[&](int col){ return double(std::min(double(int(((col+0.5)*colW)+charW/2)),double(cw))); };
            struct Span { std::wstring text; double l,r; };
            std::vector<Span> spans;
            for(size_t w=0;w<lr.words.size();++w){
                if(allLatin){
                    Span s; s.l=1e300; s.r=-1e300;
                    for(uint32_t c:lr.words[w]) appendCode(s.text,c);
                    for(int c:lr.cols[w]){ s.l=std::min(s.l,cellLeft(c)); s.r=std::max(s.r,cellRight(c)); }
                    spans.push_back(s);
                } else {
                    std::vector<Span> cells;
                    for(size_t k=0;k<lr.words[w].size();++k){
                        Span s; appendCode(s.text,lr.words[w][k]);
                        s.l=cellLeft(lr.cols[w][k]); s.r=cellRight(lr.cols[w][k]);
                        cells.push_back(s);
                    }
                    std::stable_sort(cells.begin(),cells.end(),[](const Span& a,const Span& b){ return a.l<b.l; });
                    spans.insert(spans.end(),cells.begin(),cells.end());
                }
            }
            for(size_t k=0;k+1<spans.size();++k){ // adjust_box_overlap
                if(spans[k].r>spans[k+1].l){
                    const double d=std::abs(spans[k].r-spans[k+1].l);
                    spans[k].r-=d/2;
                    spans[k+1].l+=d-d/2;
                }
            }
            // Back into the picture through the crop's perspective (reverse_rotate_crop_image).
            Quad b;
            for(size_t k=0;k<4;++k) b[k]={double(float(box[k].x)),double(float(box[k].y))};
            const double e0=dist(b[0],b[1]),e1=dist(b[1],b[2]),e2=dist(b[2],b[3]),e3=dist(b[3],b[0]);
            const double bw=std::max(e0,e2),bh=std::max(e1,e3);
            const bool vertical=bw<1e-6||std::nearbyint(bh/bw*100)/100>=1.5;
            double left=1e300,top=1e300;
            for(const auto& p:b){ left=std::min(left,p.x); top=std::min(top,p.y); }
            const int il=int(left),it=int(top);
            for(auto& p:b){ p.x=double(float(p.x-il)); p.y=double(float(p.y-it)); }
            const int icw=int(dist(b[0],b[1])),ich=int(dist(b[0],b[3]));
            const Quad std4{P2{0,0},P2{double(icw),0},P2{double(icw),double(ich)},P2{0,double(ich)}};
            const auto IM=invert3(perspective(b,std4));
            for(const auto& s:spans){
                double lo=1e300,hi=-1e300;
                for(const P2& pt:{P2{s.l,0},P2{s.r,0},P2{s.r,double(ch)},P2{s.l,double(ch)}}){
                    P2 p=pt;
                    if(vertical){ // s_rotate by -90 degrees about the origin, then shifted
                        const double a=-3.141592653589793/2;
                        const P2 r{p.x*std::cos(a)+p.y*std::sin(a),p.y*std::cos(a)-p.x*std::sin(a)};
                        p={r.x+icw,r.y};
                    }
                    const double X=IM[0]*p.x+IM[1]*p.y+IM[2],Z=IM[6]*p.x+IM[7]*p.y+IM[8];
                    const double ox=double(int(X/Z+il));
                    lo=std::min(lo,ox); hi=std::max(hi,ox);
                }
                words.push_back({s.text,float(lo),float(hi)});
            }
        }
        result.boxes.push_back(box);
        result.texts.push_back(std::move(text));
        result.scores.push_back(lr.score);
        result.words.push_back(std::move(words));
    }
    return result;
}

namespace {
std::vector<Piece> toPieces(const std::vector<Quad>& boxes,const std::vector<std::wstring>& texts,
                            const std::vector<double>& scores,const std::vector<std::vector<PieceWord>>& words,double scale){
    std::vector<Piece> pieces;
    auto blank=[](const std::wstring& s){ return std::all_of(s.begin(),s.end(),[](wchar_t c){ return isSpace(uint32_t(c)); }); };
    for(size_t i=0;i<texts.size();++i){
        if(blank(texts[i])) continue;
        Piece p;
        p.text=texts[i];
        p.score=float(scores[i]);
        for(size_t k=0;k<4;++k) p.quad[k]={float(boxes[i][k].x/scale),float(boxes[i][k].y/scale)};
        for(const auto& w:words[i]){
            if(blank(w.text)) continue;
            p.words.push_back({w.text,float(w.left/scale),float(w.right/scale)});
        }
        pieces.push_back(std::move(p));
    }
    return pieces;
}
} // namespace

std::vector<Piece> NeuralOcr::readAt(const Crop& crop,double scale) const {
    scale=std::min(scale,1.0);
    const Look l=look(crop,scale,true);
    return toPieces(l.boxes,l.texts,l.scores,l.words,scale);
}

std::vector<Piece> NeuralOcr::read(const Crop& crop){
    return read(crop,std::chrono::steady_clock::now());
}

// Letter sizes in px: the size read best, and the sizes beyond which the picture is resized.
static constexpr double bestLetters=24.0,largeLetters=30.0,smallLetters=15.0;

std::vector<Piece> NeuralOcr::read(const Crop& crop,std::chrono::steady_clock::time_point now){
    const double given=1.0;
    Look result=look(crop,scale_,true);
    if(scale_<given&&result.texts.empty()){
        // Nothing in the reduced picture - but text of an ordinary size may simply be too
        // small in it (after a title in large letters): twice a second look at full size.
        if(now>=lookAgainAt_){
            lookAgainAt_=now+std::chrono::milliseconds(500);
            const double reduced=scale_;
            scale_=given;
            result=look(crop,scale_,true);
            if(result.texts.empty()) scale_=reduced;
        }
    }
    for(int pass=0;pass<2&&!result.texts.empty();++pass){
        // The letter height from the tallest box: boxes sharing a height hug the letters of
        // words (letters ~ box / 1.1), a box of a whole line stands ~1.45 letters tall.
        std::vector<double> tops,heights;
        for(const auto& b:result.boxes){
            double t=1e300,bo=-1e300;
            for(const auto& p:b){ t=std::min(t,double(float(p.y))); bo=std::max(bo,double(float(p.y))); }
            tops.push_back(t); heights.push_back(bo-t);
        }
        const double tallest=*std::max_element(heights.begin(),heights.end());
        bool cut=false;
        for(size_t i=0;i<tops.size()&&!cut;++i) for(size_t j=i+1;j<tops.size();++j)
            if(std::abs(tops[i]-tops[j])<0.5*tallest){ cut=true; break; }
        const double letters=tallest/(cut?1.1:1.45);
        double wanted=scale_;
        if(letters>largeLetters) wanted=scale_*bestLetters/letters;
        else if(letters<smallLetters&&scale_<given) wanted=std::min(given,scale_*bestLetters/letters);
        if(std::abs(wanted-scale_)<=0.08*scale_) break;
        // (A guard: a picture brought down may lose the line it was read at -
        // a 47 px "Люсия: Да ну тебя." came back as a stray "[" - so a size that reads clearly
        // fewer letters is not kept.)
        auto lettersIn=[](const Look& l){
            size_t n=0;
            for(const auto& t:l.texts) n+=size_t(std::count_if(t.begin(),t.end(),[](wchar_t c){ return std::iswalpha(c)||c>=0x400; }));
            return n;
        };
        if(refused_>0&&std::abs(wanted-refused_)<=0.08*refused_) break; // tried already: not again every frame
        Look again=look(crop,wanted,true);
        if(10*lettersIn(again)<7*lettersIn(result)){ refused_=wanted; break; }
        refused_=0;
        scale_=wanted;
        result=std::move(again);
    }
    return toPieces(result.boxes,result.texts,result.scores,result.words,scale_);
}

std::vector<TextBox> NeuralOcr::findText(const Crop& frame,int widest) const {
    const double scale=frame.w>0?std::min(1.0,double(widest)/double(frame.w)):1.0;
    const Look l=look(frame,scale,false);
    std::vector<TextBox> out;
    for(const auto& q:l.boxes){
        double x0=1e300,y0=1e300,x1=-1e300,y1=-1e300;
        for(const auto& p:q){ x0=std::min(x0,p.x); y0=std::min(y0,p.y); x1=std::max(x1,p.x); y1=std::max(y1,p.y); }
        out.push_back({float(x0/scale),float(y0/scale),float(x1/scale),float(y1/scale)});
    }
    return out;
}

} // namespace veyra::engine::screentext
