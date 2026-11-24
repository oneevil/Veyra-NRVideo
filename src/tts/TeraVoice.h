// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Speech from text with TeraTTSv2 (TeraSpace, https://huggingface.co/TeraSpace/TeraTTSv2,
// Open RAIL-M with an attribution condition; derived from Supertone's Supertonic): fixed
// narrators, Russian and English, 44.1 kHz, on the CPU in a fraction of a second per line.
//
// Follows the reference runtime published with the model (teratts.py): the text, with "+"
// before stressed vowels and wrapped in a <ru>/<en> tag, and a narrator's style give a text
// embedding; a second network predicts how long the line lasts; a distilled flow model turns
// noise of that length into a compressed sound in eight steps; a vocoder makes the waveform.
// Portable C++ over ONNX Runtime's C API.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace veyra::tts {

class OrtRuntime;
class OrtModel;
class RuStress;

class TeraVoice {
public:
    static constexpr int sampleRate = 44100;
    struct Options {
        int threads = 4;          // CPU threads per network
        float guidance = 3.0f;    // the model's own default
        float noise = 1.0f;       // loudness of the starting noise: more is livelier, from ~1.4 speech breaks up
        uint32_t seed = 1234;     // the reference runtime's seed
        bool stressModel = true;  // the character model for words the stress dictionary lacks
        std::filesystem::path testNoise; // checks only: starting noise from a .npy file
    };

    // `folder`: the TeraTTSv2 files (models/, styles/, ruaccent/, unicode_indexer.json).
    // `cacheDirectory`: where the stress dictionary is kept in a quick-to-load form.
    TeraVoice(const std::filesystem::path& folder, const std::filesystem::path& cacheDirectory,
              const OrtRuntime& runtime, Options options);
    ~TeraVoice();

    // The narrators found in styles/, the recommended ones first.
    const std::vector<std::string>& voices() const { return voiceNames_; }
    // The user's own stress readings, UTF-8 words with "+" before the stressed vowel (RuStress).
    void setStressFixes(const std::vector<std::string>& readings);
    // The exact text the networks are given (numbers in words, stress marks, language tag);
    // empty when nothing in it can be pronounced.
    std::u32string prepare(std::u32string_view text) const;
    // How a line is said, on top of its narrator.
    struct Delivery {
        float speed = 1.0f;      // > 1 faster
        float pitch = 1.0f;      // > 1 higher, played like a tape on top of the narrator's own shift
        float liveliness = 0.0f; // added to the starting noise: more moves the pitch further within the line
        float level = 1.0f;      // loudness against the usual
        // > 0: the line may last no longer than this (seconds) - it is said faster to fit, up to
        // `maxSpeed`; a line that fits keeps `speed`.
        float fitSeconds = 0.0f;
        float maxSpeed = 1.35f;
    };
    // Mono float PCM at 44.1 kHz; empty when there is nothing to pronounce. `speed` > 1 is faster.
    std::vector<float> synthesize(std::string_view utf8Text, std::string_view voice, float speed = 1.0f) const;
    std::vector<float> synthesize(std::string_view utf8Text, std::string_view voice, const Delivery& delivery) const;

private:
    struct Style {
        std::vector<float> ttl; // [1, 50, 256]
        std::vector<float> dp;  // [1, 8, 16]
    };
    float predicted(const std::u32string& prepared, const Style& style) const; // seconds the model gives it, as is
    std::vector<float> phrase(const std::u32string& prepared, const Style& style, float speed, float modelSeconds,
                              float noise) const;
    std::vector<int64_t> ids(std::u32string_view text) const;

    Options options_;
    std::vector<int32_t> indexer_; // code point -> token, -1 if unknown
    std::unique_ptr<RuStress> stress_;
    std::unique_ptr<OrtModel> encoder_, duration_, sampler_, vocoder_;
    std::unordered_map<std::string, Style> styles_;
    std::vector<std::string> voiceNames_;
};

} // namespace veyra::tts
