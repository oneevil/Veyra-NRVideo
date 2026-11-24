// SPDX-License-Identifier: GPL-3.0-only
#include "TeraVoice.h"
#include "Narrators.h"
#include "Numbers.h"
#include "Ort.h"
#include "RuStress.h"
#include "Utf8.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>

namespace veyra::tts {
namespace {

constexpr int framesPerLatent = 3072;    // samples of sound in one step of the compressed form
constexpr float referencePace = 1.05f;   // the reference speaks this much faster than predicted
constexpr size_t longestPiece = 220;     // letters per synthesis; the model was trained on clips up to 25 s
constexpr float targetLevel = 0.08f;     // RMS every line is brought to: the narrators differ threefold
constexpr int edgeFade = 220;            // 5 ms fade at both ends against clicks
constexpr int pieceGap = TeraVoice::sampleRate / 8;

// Narrators in the order they are offered: the model's authors recommend ru_m5 and ru_f1.
constexpr const char* knownVoices[]{"ru_m5", "ru_f1", "ru_m1", "ru_f2", "eng_m4", "eng_f5", "eng_m3", "eng_f3"};

std::string readFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + file.string());
    std::ostringstream data;
    data << in.rdbuf();
    return std::move(data).str();
}

// A float32 (or float64) .npy array, flattened; `expected` elements or an exception.
std::vector<float> readNpy(const std::filesystem::path& file, size_t expected) {
    const auto data = readFile(file);
    if (data.size() < 10 || std::memcmp(data.data(), "\x93NUMPY", 6) != 0)
        throw std::runtime_error("not a .npy file: " + file.string());
    const int major = uint8_t(data[6]);
    size_t headerLength = 0, offset = 0;
    if (major == 1) { headerLength = uint8_t(data[8]) | (uint8_t(data[9]) << 8); offset = 10; }
    else {
        if (data.size() < 12) throw std::runtime_error("short .npy file: " + file.string());
        headerLength = uint8_t(data[8]) | (uint8_t(data[9]) << 8) | (size_t(uint8_t(data[10])) << 16) |
                       (size_t(uint8_t(data[11])) << 24);
        offset = 12;
    }
    const std::string_view header(data.data() + offset, std::min(headerLength, data.size() - offset));
    if (header.find("'fortran_order': True") != std::string_view::npos)
        throw std::runtime_error("unsupported .npy layout: " + file.string());
    const bool doubles = header.find("<f8") != std::string_view::npos;
    if (!doubles && header.find("<f4") == std::string_view::npos)
        throw std::runtime_error("unsupported .npy type: " + file.string());
    const char* values = data.data() + offset + headerLength;
    const size_t bytes = data.size() - offset - headerLength;
    if (!expected) expected = bytes / (doubles ? 8 : 4); // all of it
    std::vector<float> out(expected);
    if (bytes < expected * (doubles ? 8 : 4)) throw std::runtime_error("short .npy file: " + file.string());
    for (size_t i = 0; i < expected; ++i) {
        if (doubles) { double v; std::memcpy(&v, values + i * 8, 8); out[i] = float(v); }
        else std::memcpy(&out[i], values + i * 4, 4);
    }
    return out;
}

std::vector<int32_t> readIndexer(const std::filesystem::path& file) {
    const auto json = readFile(file);
    std::vector<int32_t> table;
    table.reserve(65536);
    const char* p = json.c_str();
    while (*p && *p != '[') ++p;
    while (*p) {
        while (*p && *p != '-' && (*p < '0' || *p > '9') && *p != ']') ++p;
        if (!*p || *p == ']') break;
        char* end = nullptr;
        table.push_back(int32_t(std::strtol(p, &end, 10)));
        p = end;
    }
    if (table.size() != 65536) throw std::runtime_error("unicode_indexer.json must have 65536 entries");
    return table;
}

// Punctuation and letters the model has no token for, as ones it knows.
void appendNormalized(std::u32string& out, char32_t c) {
    switch (c) {
    case U'…': out += U"..."; return;
    case U'—': case U'–': case U'‒': case U'−': case U'‐': out.push_back(U'-'); return;
    case U'“': case U'”': case U'„': case U'‟': case U'″': out.push_back(U'"'); return;
    case U'‘': case U'’': case U'‚': case U'`': case U'´': case U'′': out.push_back(U'\''); return;
    case U'\t': case U'\n': case U'\r': case 0xA0: case 0x2009: case 0x202F: out.push_back(U' '); return;
    case U'¡': case U'¿': return;
    default: break;
    }
    // Latin letters with accents read as their base letter (the model's alphabet is plain Latin).
    static constexpr std::u32string_view accented = U"ÀÁÂÃÄÅàáâãäåÈÉÊËèéêëÌÍÎÏìíîïÑñÒÓÔÕÖòóôõöÙÚÛÜùúûüÝýÿÇç";
    static constexpr std::u32string_view plain    = U"AAAAAAaaaaaaEEEEeeeeIIIIiiiiNnOOOOOoooooUUUUuuuuYyyCc";
    if (const auto at = accented.find(c); at != std::u32string_view::npos) { out.push_back(plain[at]); return; }
    out.push_back(c);
}

bool isMark(char32_t c) { return c == U',' || c == U'.' || c == U'!' || c == U'?' || c == U';' || c == U':'; }

// Sentences, cut where a line is longer than the model takes in one go.
std::vector<std::u32string> pieces(const std::u32string& text) {
    if (text.size() <= longestPiece) return {text};
    std::vector<std::u32string> out;
    std::u32string current;
    for (size_t i = 0; i < text.size(); ++i) {
        current.push_back(text[i]);
        const bool end = (text[i] == U'.' || text[i] == U'!' || text[i] == U'?') && i + 1 < text.size() && text[i + 1] == U' ';
        if ((end && current.size() > 20) || current.size() >= longestPiece) {
            out.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) out.push_back(std::move(current));
    return out;
}

} // namespace

void TeraVoice::setStressFixes(const std::vector<std::string>& readings) {
    std::vector<std::u32string> wide;
    for (const auto& r : readings) wide.push_back(decodeUtf8(r));
    stress_->setUserFixes(wide);
}

TeraVoice::TeraVoice(const std::filesystem::path& folder, const std::filesystem::path& cacheDirectory,
                     const OrtRuntime& runtime, Options options)
    : options_(options), indexer_(readIndexer(folder / "unicode_indexer.json")) {
    const auto models = folder / "models";
    encoder_ = std::make_unique<OrtModel>(runtime, models / "text_encoder.onnx", options.threads);
    duration_ = std::make_unique<OrtModel>(runtime, models / "duration_predictor.onnx", options.threads);
    sampler_ = std::make_unique<OrtModel>(runtime, models / "sampler_distilled_cfg3_8step.onnx", options.threads);
    vocoder_ = std::make_unique<OrtModel>(runtime, models / "vocoder.onnx", options.threads);
    stress_ = std::make_unique<RuStress>(folder / "ruaccent",
        cacheDirectory.empty() ? std::filesystem::path{} : cacheDirectory / "teratts-accents.bin",
        options.stressModel ? &runtime : nullptr);
    for (const char* name : knownVoices) {
        const auto dir = folder / "styles" / name;
        std::error_code error;
        if (!std::filesystem::exists(dir / "style_ttl.npy", error)) continue;
        styles_[name] = Style{readNpy(dir / "style_ttl.npy", 50 * 256), readNpy(dir / "style_dp.npy", 8 * 16)};
    }
    // Every narrator (Narrators.h) whose style is here, in the order they are handed out.
    for (const auto& narrator : narrators())
        if (styles_.count(narrator.style)) voiceNames_.push_back(narrator.id);
    if (voiceNames_.empty()) throw std::runtime_error("no TeraTTS narrators in " + (folder / "styles").string());
}

TeraVoice::~TeraVoice() = default;

std::u32string TeraVoice::prepare(std::u32string_view input) const {
    std::u32string text;
    text.reserve(input.size() + 16);
    for (const char32_t c : input) appendNormalized(text, c);
    const bool russian = std::any_of(text.begin(), text.end(), isCyrillic);
    text = spellNumbers(text, russian);

    // Marks: any run of dots is three (OCR often reads two or four); a space after a mark
    // that is glued to the next word, but none inside a run of marks ("..." must not become
    // ". . .", which the model reads as three sentence ends with pauses).
    std::u32string spaced;
    spaced.reserve(text.size() + 16);
    for (size_t i = 0; i < text.size(); ++i) {
        const char32_t c = text[i];
        if (c == U'.' && i + 1 < text.size() && text[i + 1] == U'.') {
            size_t end = i;
            while (end < text.size() && text[end] == U'.') ++end;
            spaced += U"...";
            i = end - 1;
        } else {
            spaced.push_back(c);
        }
        if (isMark(spaced.back()) && i + 1 < text.size() && !isSpace(text[i + 1]) && !isMark(text[i + 1]) &&
            text[i + 1] != U'"' && text[i + 1] != U')' && text[i + 1] != U'»')
            spaced.push_back(U' ');
    }

    // Characters the model has no token for become spaces; spaces collapse.
    std::u32string kept;
    kept.reserve(spaced.size());
    for (const char32_t c : spaced) {
        const char32_t base = (c == U'й' || c == U'Й' || c == U'ё' || c == U'Ё') ? (c == U'й' ? U'и' : c == U'Й' ? U'И' : c == U'ё' ? U'е' : U'Е') : c;
        const bool known = base < 65536 && indexer_[base] >= 0 && base != U'<' && base != U'>';
        const char32_t put = known ? c : U' ';
        if (isSpace(put) && (kept.empty() || kept.back() == U' ')) continue;
        kept.push_back(isSpace(put) ? U' ' : put);
    }
    while (!kept.empty() && kept.back() == U' ') kept.pop_back();
    if (std::none_of(kept.begin(), kept.end(), isLetter)) return {};

    if (russian) kept = stress_->mark(kept);
    std::u32string tagged = russian ? U"<ru>" : U"<en>";
    // The model's table holds й and ё decomposed (NFKD), as it was trained.
    for (const char32_t c : kept) {
        switch (c) {
        case U'й': tagged += U"й"; break;
        case U'Й': tagged += U"Й"; break;
        case U'ё': tagged += U"ё"; break;
        case U'Ё': tagged += U"Ё"; break;
        default: tagged.push_back(c); break;
        }
    }
    tagged += russian ? U"</ru>" : U"</en>";
    return tagged;
}

std::vector<int64_t> TeraVoice::ids(std::u32string_view text) const {
    std::vector<int64_t> out;
    out.reserve(text.size());
    for (const char32_t c : text) {
        const int32_t token = c < 65536 ? indexer_[c] : -1;
        if (token >= 0) out.push_back(token);
    }
    return out;
}

// How long the model would say a phrase (stress marks taken out, as the reference runtime does).
float TeraVoice::predicted(const std::u32string& prepared, const Style& style) const {
    std::u32string plain;
    for (const char32_t c : prepared) if (c != U'+') plain.push_back(c);
    const auto plainIds = ids(plain);
    if (plainIds.empty()) return 0.0f;
    const auto plainLength = int64_t(plainIds.size());
    const auto plainTensor = Tensor::of({1, plainLength}, plainIds);
    const auto plainMask = Tensor::of({1, 1, plainLength}, std::vector<float>(size_t(plainLength), 1.0f));
    const auto dp = Tensor::of({1, 8, 16}, style.dp);
    const auto out = duration_->run({{"text_ids", &plainTensor}, {"style_dp", &dp}, {"text_mask", &plainMask}},
                                    "output");
    return out.floats.empty() ? 0.0f : out.floats[0];
}

std::vector<float> TeraVoice::phrase(const std::u32string& prepared, const Style& style, float speed, float modelSeconds,
                                     float noiseLevel) const {
    const auto textIds = ids(prepared);
    if (textIds.empty() || !(modelSeconds > 0.0f)) return {};
    const auto length = int64_t(textIds.size());

    const auto textTensor = Tensor::of({1, length}, textIds);
    const auto textMask = Tensor::of({1, 1, length}, std::vector<float>(size_t(length), 1.0f));
    const auto ttl = Tensor::of({1, 50, 256}, style.ttl);
    const auto embedding = encoder_->run({{"text_ids", &textTensor}, {"style_ttl", &ttl}, {"text_mask", &textMask}},
                                         "output");

    const float seconds = modelSeconds / std::max(0.5f, speed) / referencePace;
    if (!std::isfinite(seconds) || seconds <= 0.0f) return {};
    const auto frames = std::max<int64_t>(1, int64_t(std::ceil(seconds * sampleRate / framesPerLatent)));

    std::vector<float> noise(size_t(144 * frames));
    if (!options_.testNoise.empty()) {
        // Checks: the reference runtime's own noise, so that the sound can be compared sample by sample.
        const auto fixed = readNpy(options_.testNoise, 0);
        if (fixed.size() < noise.size()) throw std::runtime_error("test noise is too short");
        std::copy_n(fixed.begin(), noise.size(), noise.begin());
    } else {
        std::mt19937 random(options_.seed);
        std::normal_distribution<float> normal(0.0f, 1.0f);
        for (auto& value : noise) value = normal(random) * noiseLevel;
    }
    const auto latentIn = Tensor::of({1, 144, frames}, std::move(noise));
    const auto latentMask = Tensor::of({1, 1, frames}, std::vector<float>(size_t(frames), 1.0f));
    const auto guidance = Tensor::of({1}, std::vector<float>{options_.guidance});
    auto embeddingIn = embedding;
    const auto latent = sampler_->run({{"initial_latent", &latentIn}, {"text_emb", &embeddingIn}, {"style_ttl", &ttl},
                                       {"latent_mask", &latentMask}, {"text_mask", &textMask}, {"guidance", &guidance}},
                                      "latent");
    auto wave = vocoder_->run({{"latent", &latent}}, "output").floats;
    wave.resize(std::min(wave.size(), size_t(std::lround(seconds * sampleRate))));
    return wave;
}

std::vector<float> TeraVoice::synthesize(std::string_view utf8Text, std::string_view voice, float speed) const {
    Delivery delivery;
    delivery.speed = speed;
    return synthesize(utf8Text, voice, delivery);
}

std::vector<float> TeraVoice::synthesize(std::string_view utf8Text, std::string_view voice,
                                         const Delivery& delivery) const {
    float speed = std::clamp(delivery.speed, 0.5f, 2.0f);
    // A narrator is a style and a tape-like shift (Narrators.h); a plain style name works too.
    const Narrator* narrator = findNarrator(voice);
    float ratio = narrator ? narrator->ratio : 1.0f;
    auto style = styles_.find(narrator ? narrator->style : std::string(voice));
    if (style == styles_.end()) {
        ratio = 1.0f;
        const Narrator* first = findNarrator(voiceNames_.front());
        style = styles_.find(first ? first->style : voiceNames_.front());
    }
    // The "eng" narrators were recorded speaking English: in Russian they keep a slight accent
    // and a slow pace, so they speak a fifth briskier. A shifted narrator is
    // spoken `ratio` times slower or faster first, so that the shift leaves the pace as it was.
    const float brisk = style->first.starts_with("eng_") ? 1.0f / 0.8f : 1.0f;
    // The line's own pitch (how it is said) goes on top of the narrator's, the same way.
    ratio *= std::clamp(delivery.pitch, 0.85f, 1.15f);
    const float noise = std::clamp(options_.noise + delivery.liveliness, 0.6f, 1.4f);
    // Mostly words no dictionary knows: the OCR misread something that is not a subtitle.
    int counted = 0;
    const float known = stress_->knownShare(decodeUtf8(utf8Text), counted);
    if ((counted >= 3 && known < 0.7f) || (counted == 2 && known < 0.5f)) return {};
    const auto prepared = prepare(decodeUtf8(utf8Text));
    if (prepared.empty()) return {};
    // A long line is spoken sentence by sentence, each with its own tag.
    const bool russian = prepared.starts_with(U"<ru>");
    const auto body = prepared.substr(4, prepared.size() - 9);
    std::vector<std::pair<std::u32string, float>> parts;
    float total = 0.0f; // seconds, as the model would say it at speed 1
    for (const auto& piece : pieces(body)) {
        auto tagged = (russian ? U"<ru>" : U"<en>") + piece + (russian ? U"</ru>" : U"</en>");
        const float length = predicted(tagged, style->second);
        if (!(length > 0.0f) || !std::isfinite(length)) continue;
        total += length;
        parts.emplace_back(std::move(tagged), length);
    }
    if (parts.empty()) return {};
    // Too long for the time it has: said faster (the pauses between sentences stay).
    if (delivery.fitSeconds > 0.0f) {
        const float gaps = float(parts.size() - 1) * float(pieceGap) / float(sampleRate);
        const float lasts = total / (speed * brisk) / referencePace + gaps;
        const float room = std::max(0.3f, delivery.fitSeconds - gaps);
        if (lasts > delivery.fitSeconds)
            speed = std::max(speed, std::min(std::max(speed, delivery.maxSpeed), speed * (lasts - gaps) / room));
    }
    speed = speed * brisk / ratio;
    std::vector<float> sound;
    for (const auto& [tagged, length] : parts) {
        auto part = phrase(tagged, style->second, speed, length, noise);
        if (part.empty()) continue;
        if (!sound.empty()) sound.insert(sound.end(), size_t(pieceGap), 0.0f);
        sound.insert(sound.end(), part.begin(), part.end());
    }
    if (sound.empty()) return sound;
    if (ratio != 1.0f) sound = playFaster(sound, ratio);
    double energy = 0.0;
    float peak = 0.0f;
    for (const float s : sound) { energy += double(s) * s; peak = std::max(peak, std::abs(s)); }
    const float rms = float(std::sqrt(energy / double(sound.size())));
    if (rms > 0.0f) {
        const float level = std::clamp(delivery.level, 0.5f, 1.6f);
        const float gain = std::min(targetLevel * level / rms, peak > 0.0f ? 0.95f / peak : 1.0f);
        for (auto& s : sound) s *= gain;
    }
    const size_t fade = std::min(sound.size() / 2, size_t(edgeFade));
    for (size_t i = 0; i < fade; ++i) {
        const float k = float(i) / float(fade);
        sound[i] *= k;
        sound[sound.size() - 1 - i] *= k;
    }
    return sound;
}

} // namespace veyra::tts
