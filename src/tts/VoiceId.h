// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Who is speaking a subtitle line, judged from the game's own sound while the line is on
// screen - for subtitles that carry no names:
//
//  * Silero VAD keeps only the parts of the clip where someone talks (not music, effects);
//  * a man or a woman: the ECAPA-TDNN voice gender classifier (Jaesung Huh, MIT) - from the
//    voice itself, since pitch fails on excited men;
//  * which person: WeSpeaker ResNet221 (Apache-2.0) voice fingerprints. When a line has ended,
//    its whole speech is fingerprinted and all the lines of the session are grouped afresh
//    (average linkage), each group a voice ("Голос 1", "Голос 2", ...) - regrouping everything
//    can undo an early mistake. When a line starts, its opening is only matched against the
//    voices there are, with a small bonus for voices heard in the last few lines.
//
// Portable C++ over ONNX Runtime; 16 kHz mono float PCM in.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace veyra::tts {

class OrtRuntime;
class OrtModel;

class SpeechDetector {
public:
    SpeechDetector(const OrtRuntime& runtime, const std::filesystem::path& sileroModel);
    ~SpeechDetector();
    // The speech of a clip, joined; `brief`: keep short bursts too (the opening of a line).
    std::vector<float> speech(const std::vector<float>& wave16k, bool brief) const;

private:
    std::unique_ptr<OrtModel> model_;
    bool v5_ = false; // input/state/sr (Silero 5) rather than x/h/c (Silero 4, sherpa-onnx's export)
};

class GenderModel {
public:
    // `folder` holds the author's model.safetensors; the graph (src/tts/GenderGraph.inc) is
    // written next to it, since its weights are read from that file by offset.
    GenderModel(const OrtRuntime& runtime, const std::filesystem::path& folder, int threads);
    ~GenderModel();
    // Above zero a woman, below a man; the further from zero, the surer. Nothing for a clip
    // under a quarter of a second.
    std::optional<float> score(const std::vector<float>& speech16k) const;

private:
    std::unique_ptr<OrtModel> model_;
};

class VoicePrint {
public:
    VoicePrint(const OrtRuntime& runtime, const std::filesystem::path& model, int threads);
    ~VoicePrint();
    std::vector<float> embed(const std::vector<float>& speech16k) const; // unit length, 256 values

private:
    std::unique_ptr<OrtModel> model_;
};

// The voices of a session: lines grouped by their fingerprints.
class VoiceGroups {
public:
    struct Match { std::string voice; std::string closest; float likeness = 0.0f; };
    struct Credited {
        std::string voice;                                            // the voice the line belongs to now
        bool isNew = false;                                           // the line founded it
        std::vector<std::pair<std::string, std::optional<bool>>> born; // other new voices (name, a woman's?)
        std::vector<std::string> gone;                                // voices joined into another
    };
    // The opening of a line: which known voice it is, if it is clear.
    Match match(const std::vector<float>& embedding) const;
    // A whole finished line (and whether its voice is a woman's, when known).
    Credited assign(const std::vector<float>& embedding, std::optional<bool> female);
    std::optional<bool> female(const std::string& voice) const; // the majority of its lines
    void clear();

private:
    struct Line { std::vector<float> embedding; std::string voice; std::optional<bool> female; };
    std::vector<Line> lines_;
    std::vector<std::pair<std::string, std::vector<float>>> centroids_;
    std::vector<std::string> recent_; // voices of the last few lines, newest last
    int count_ = 0;
};

// All of it, as the voice worker uses it.
class VoiceListener {
public:
    struct Heard {
        std::optional<bool> female;
        std::optional<float> score; // the gender network's answer: above zero a woman; the further from zero, the surer
        std::string voice;     // empty: not recognised (yet)
        float speechSeconds = 0.0f;
        std::optional<float> pitch; // typical pitch of the voice, Hz (picks a narrator close to it)
        // How the line is said, from its sound: -1 calm or quiet .. 0 as usual .. 1 loud, raised,
        // excited - by its loudness against this session's lines and its pitch against the usual
        // pitch of men's or women's voices heard in it. Nothing when too little was heard.
        std::optional<float> arousal;
    };
    VoiceListener(const OrtRuntime& runtime, const std::filesystem::path& silero,
                  const std::filesystem::path& genderFolder, const std::filesystem::path& speakerModel,
                  int threads, bool recognise);
    ~VoiceListener();
    // The opening of a line (from when it appeared until now).
    Heard opening(const std::vector<float>& wave16k);
    // A line that has left the screen: its whole sound, to learn the voices from. `speechSeconds`:
    // how long its speaker talked in it (the game's pace of speech).
    VoiceGroups::Credited lineEnded(const std::vector<float>& wave16k, std::optional<bool>& female,
                                    float* speechSeconds = nullptr);
    void reset();

private:
    std::string groupsMatch(const std::vector<float>& speech);
    std::optional<float> arousal(const std::vector<float>& speech, std::optional<bool> female, std::optional<float> pitch) const;
    void learnUsual(const std::vector<float>& speech, std::optional<bool> female);

    SpeechDetector detector_;
    GenderModel gender_;
    std::unique_ptr<VoicePrint> print_;
    VoiceGroups groups_;
    // This session's usual: loudness of speech (dB) of men, women and everyone, and pitch of
    // men's and women's voices (Hz).
    std::optional<float> usualLoudness_[3], usualPitch_[2];
};

// Loudness of a clip in dB (RMS against full scale); very quiet for silence.
float loudnessDb(const std::vector<float>& wave);
// How a line is said against the usual (VoiceListener::Heard::arousal): `louderDb` its loudness
// above the usual, `higherSemitones` its pitch above the usual for its sex.
float arousalOf(float louderDb, float higherSemitones);

} // namespace veyra::tts
