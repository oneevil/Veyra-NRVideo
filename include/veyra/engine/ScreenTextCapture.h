#pragma once
// Optional on-screen text source for live translation (game subtitles from a
// capture card, PS5 Remote Play, Xbox or PC streaming, or a video). While it is
// on, a region of the presented picture is copied into a small readback buffer a
// few times a second and read by Windows OCR on its own worker thread
// (ScreenText.h holds the reading logic, ported from VI Translator).
// Like VirtualCamera it is a compatibility boundary that only runs while enabled:
// record() never waits on the GPU, the worker or a lock, at most one read is in
// flight per slot, and a frame is skipped while the previous one is still being
// read. Off, it records nothing.
// Engine thread: record/submitted/discarded. Any thread: start/stop/sessionEnded/status.
// Kept free of Qt-sensitive names: EngineController.h is included after Qt.
#include "veyra/engine/PresentCapture.h"
#include <cstdint>
#include <memory>
#include <string>

namespace veyra::engine {

struct ScreenTextOptions {
    // The region read, as fractions of the picture's width and height.
    float x=0.08f,y=0.70f,w=0.84f,h=0.27f;
    bool centeredOnly=true;      // subtitles: keep only the centred block of words
    std::wstring language=L"en"; // BCP-47 language of the text on screen
    uint32_t intervalMs=250;     // time between reads
    // How the game shows who speaks: 0 "Name: text" or a name in its own colour, 1 also a name
    // on a line of its own above the speech, 2 never (no name is looked for).
    int nameStyle=0;
    // The neural reader (PP-OCRv5) instead of Windows' own: its model files and the ONNX Runtime
    // library (all empty: Windows' reader).
    std::wstring detector,recognizer,runtime;
    int game=0;               // a game's own rules (neural reader): 0 none, 1 GTA VI
    std::wstring wordList;    // the stress dictionary's quick copy: real words for choosing readings
    bool operator==(const ScreenTextOptions&) const=default;
};

struct ScreenTextStatus {
    bool active=false;    // enabled
    bool ready=false;     // the OCR engine for the language is loaded
    bool failed=false;    // see error
    std::wstring text;    // the settled line on screen now; empty: none
    uint64_t sequence=0;  // changes whenever text does
    // When that line first showed up, by steady_clock in microseconds (the clock of
    // sink::recordingClockUs); 0 when the reader does not know (Windows' reader).
    int64_t appearedUs=0;
    uint64_t reads=0;     // OCR reads since enabled
    double readMs=0;      // duration of the last read (all passes)
    std::wstring language;// the recognizer's language tag
    std::wstring error;   // Chinese source text, translated by the UI catalog
    // The last read, for the settings preview (shown, never logged): the line it found
    // after cleanup (before it settles; empty: none) and how many words the OCR returned.
    std::wstring reading;
    uint32_t words=0;
    bool neural=false;    // the neural reader is reading (it loaded)
    uint32_t clippedLines=0; // lines shown since the area was set that reached its left or right side
    // Looking for where the subtitles are (findSubtitles): 0 idle, 1 looking, 2 found, 3 nothing
    // seen, 4 failed; the seconds still to look; the band found, as fractions of the picture.
    int findState=0,findSecondsLeft=0;
    float foundX=0,foundY=0,foundW=0,foundH=0;
    uint64_t findSequence=0; // changes when a search ends
};
// Diagnostics: with the environment variable VEYRA_SCREEN_TEXT_DEBUG set, every read's
// words and a few images of the read area go to logs/screen-text (opt-in only; the
// normal log carries counts and timing, never the text).

class ScreenTextCapture final : public PresentCapture {
public:
    ScreenTextCapture();
    ~ScreenTextCapture() override;
    ScreenTextCapture(const ScreenTextCapture&)=delete;
    ScreenTextCapture& operator=(const ScreenTextCapture&)=delete;
    // Enables reading, or applies new options when already enabled.
    void start(const ScreenTextOptions& options);
    void stop();
    // Watches the whole picture for `seconds` with the neural reader's text finder and reports
    // the band where subtitles appear (status().find*); works whether reading is on or not.
    void findSubtitles(const std::wstring& detector,const std::wstring& recognizer,const std::wstring& runtime,int seconds);
    // The picture changed source: the shown line and any read in flight are dropped.
    void sessionEnded();
    ScreenTextStatus status() const;
    bool record(ID3D12GraphicsCommandList* list,const Frame& frame) override;
    void submitted(ID3D12Fence* fence,uint64_t value) override;
    void discarded() override;
private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace veyra::engine
