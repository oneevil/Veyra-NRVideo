#pragma once
// Neural reading of on-screen text: PP-OCRv5 (a DBNet text finder and a CTC line reader)
// run through ONNX Runtime, with the pre- and post-processing of RapidOCR 3.10.0 written
// out in plain C++ (no OpenCV), so it reads like RapidOCR does with the settings the
// subtitle readers use: no orientation network, the frame searched at its own size
// (largest side up to 1440 px), lower thresholds (box 0.3, pixel 0.2), word places on.
// On top of that it keeps the letters near the size the reader reads best (see read()).
// Portable C++: no Qt, no Windows.
#include "veyra/engine/ScreenText.h"
#include "veyra/engine/TextPieces.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace veyra::tts {
class OrtRuntime;
class OrtModel;
}

namespace veyra::engine::screentext {

// Where text was found, axis-aligned, in the given picture's pixels.
struct TextBox {
    float left=0,top=0,right=0,bottom=0;
};

struct NeuralOcrSettings {
    int threads=4;           // intra-op threads of each network
    float pixelThresh=0.2f;  // DB: a pixel is text above this
    float boxThresh=0.3f;    // DB: a region is kept when its mean is above this
    float unclipRatio=1.6f;  // DB: how far a region is grown back
    int limitSideLen=1440;   // the finder's input: largest side at most this
    float textScore=0.5f;    // lines read with less confidence are dropped
};

class NeuralOcr {
public:
    // detector: ch_PP-OCRv5_det_mobile.onnx; reader: <script>_PP-OCRv5_rec_mobile.onnx
    // (eslav for Russian, en, latin, ...): the reader's alphabet is read from the model
    // file's metadata ("character"). Throws tts::OrtError when a model cannot be loaded or
    // carries no alphabet. The runtime must outlive this object.
    NeuralOcr(const tts::OrtRuntime& runtime,const std::filesystem::path& detector,
              const std::filesystem::path& reader,NeuralOcrSettings settings={});
    ~NeuralOcr();
    NeuralOcr(const NeuralOcr&)=delete;
    NeuralOcr& operator=(const NeuralOcr&)=delete;

    // Every stretch of text in the crop with its words, in crop pixels. The crop is looked
    // at brought down so that its letters are about 24 px high when they are larger than
    // 30 px (the scale is kept for the next crops: a game keeps the size of its subtitles);
    // while a reduced picture shows nothing, it is looked at in its own size again at most
    // twice a second. `now` drives that pace (steady clock by default).
    std::vector<Piece> read(const Crop& crop);
    std::vector<Piece> read(const Crop& crop,std::chrono::steady_clock::time_point now);
    // One look at the crop resized by `scale` (<= 1), nothing adaptive; places in crop pixels.
    std::vector<Piece> readAt(const Crop& crop,double scale) const;
    // The scale read() uses now (1 = the crop's own size), and back to the crop's own size.
    double scale() const { return scale_; }
    void resetScale() { scale_=1.0; refused_=0; lookAgainAt_={}; }

    // Text finding only, for a whole frame (the search for the subtitle area): the frame is
    // brought down to at most `widest` px across first; boxes in the frame's own pixels.
    std::vector<TextBox> findText(const Crop& frame,int widest=1280) const;

    // The reader's alphabet (index 0 is the CTC blank, the last one a space).
    const std::vector<std::wstring>& alphabet() const { return alphabet_; }

private:
    struct Look;
    Look look(const Crop& crop,double scale,bool recognise) const;

    NeuralOcrSettings settings_;
    std::unique_ptr<tts::OrtModel> detector_,reader_;
    std::string detInput_,detOutput_,recInput_,recOutput_;
    std::vector<std::wstring> alphabet_;
    std::vector<uint32_t> alphabetCodes_; // one code point per entry (0 for the blank)
    double scale_=1.0;
    double refused_=0; // a size tried and found to read fewer letters (not tried again)
    std::chrono::steady_clock::time_point lookAgainAt_{};
};

// The image operations, exposed for tests. BGR8, rows packed.
namespace ocrimage {
struct Bgr {
    int w=0,h=0;
    std::vector<uint8_t> px;
};
Bgr fromRgba(const Crop& crop);
// As cv2.resize(INTER_LINEAR) to w x h.
Bgr resizeLinear(const Bgr& src,int w,int h);
// As cv2.resize(None, fx=f, fy=f, INTER_AREA), f < 1.
Bgr resizeArea(const Bgr& src,double f);
} // namespace ocrimage

} // namespace veyra::engine::screentext
