// Command-line check of the neural text reader against RapidOCR: reads binary PPM (P6)
// pictures and prints what is found, one line per piece (text, score, corners, words).
//   NeuralOcrCheck <onnxruntime library> <det.onnx> <rec.onnx> [--find] [--at SCALE]
//                  [--step MS] picture.ppm...
// Default: the adaptive read(); several pictures are read in turn by one reader, the clock
// moving on MS (default 160) between them. --find: text finding only. --at: one plain look.
#include "../../src/engine/NeuralOcr.h"
#include "../../src/tts/Ort.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

using namespace veyra::engine::screentext;

static bool loadPpm(const std::string& file,Crop& crop){
    std::ifstream in(file,std::ios::binary);
    std::string magic; int w=0,h=0,max=0;
    if(!(in>>magic>>w>>h>>max)||magic!="P6"||max!=255) return false;
    in.get();
    std::string rgb(size_t(w)*h*3,'\0');
    if(!in.read(rgb.data(),std::streamsize(rgb.size()))) return false;
    crop.w=uint32_t(w); crop.h=uint32_t(h);
    crop.rgba.resize(size_t(w)*h*4);
    for(size_t i=0;i<size_t(w)*h;++i){
        for(int c=0;c<3;++c) crop.rgba[i*4+size_t(c)]=uint8_t(rgb[i*3+size_t(c)]);
        crop.rgba[i*4+3]=255;
    }
    return true;
}

static std::string utf8(const std::wstring& s){
    std::string o;
    for(wchar_t wc:s){
        uint32_t c=uint32_t(wc);
        if(c<0x80) o+=char(c);
        else if(c<0x800){ o+=char(0xC0|(c>>6)); o+=char(0x80|(c&0x3F)); }
        else if(c<0x10000){ o+=char(0xE0|(c>>12)); o+=char(0x80|((c>>6)&0x3F)); o+=char(0x80|(c&0x3F)); }
        else { o+=char(0xF0|(c>>18)); o+=char(0x80|((c>>12)&0x3F)); o+=char(0x80|((c>>6)&0x3F)); o+=char(0x80|(c&0x3F)); }
    }
    return o;
}

int main(int argc,char** argv){
    if(argc<5){ std::cerr<<"usage: NeuralOcrCheck ort-lib det.onnx rec.onnx [--find] [--at S] [--step MS] pictures...\n"; return 2; }
    try{
        veyra::tts::OrtRuntime runtime(argv[1]);
        NeuralOcr ocr(runtime,argv[2],argv[3]);
        bool find=false; double at=0; int step=160;
        auto now=std::chrono::steady_clock::now();
        for(int i=4;i<argc;++i){
            const std::string a=argv[i];
            if(a=="--find"){ find=true; continue; }
            if(a=="--at"&&i+1<argc){ at=std::stod(argv[++i]); continue; }
            if(a=="--step"&&i+1<argc){ step=std::stoi(argv[++i]); continue; }
            Crop crop;
            if(!loadPpm(a,crop)){ std::cerr<<"cannot read "<<a<<"\n"; return 1; }
            const auto t0=std::chrono::steady_clock::now();
            std::printf("# %s\n",a.c_str());
            if(find){
                const auto boxes=ocr.findText(crop);
                const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();
                for(const auto& b:boxes) std::printf("box %.1f %.1f %.1f %.1f\n",b.left,b.top,b.right,b.bottom);
                std::printf("ms %.1f\n",ms);
                continue;
            }
            const auto pieces=at>0?ocr.readAt(crop,at):ocr.read(crop,now);
            const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();
            for(const auto& p:pieces){
                std::printf("piece\t%s\t%.5f\t",utf8(p.text).c_str(),p.score);
                for(const auto& c:p.quad) std::printf("%.1f,%.1f ",c[0],c[1]);
                std::printf("\t");
                for(const auto& w:p.words) std::printf("%s[%.1f-%.1f] ",utf8(w.text).c_str(),w.left,w.right);
                std::printf("\n");
            }
            std::printf("scale %.4f\nms %.1f\n",ocr.scale(),ms);
            now+=std::chrono::milliseconds(step);
        }
    } catch(const std::exception& e){
        std::cerr<<"error: "<<e.what()<<"\n";
        return 1;
    }
    return 0;
}
