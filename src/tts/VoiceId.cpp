// SPDX-License-Identifier: GPL-3.0-only
#include "VoiceId.h"
#include "Narrators.h"
#include "Features.h"
#include "Ort.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <numeric>

namespace veyra::tts {
namespace {

#include "GenderGraph.inc"

constexpr int rate = 16000;
constexpr int window = 512;                 // 32 ms per VAD decision
constexpr float speechStarts = 0.5f, speechEnds = 0.35f;
constexpr float joinAbove = 0.35f;          // lines are grouped while this alike on average
constexpr float matchAbove = 0.30f;         // an opening is taken for a voice this alike
constexpr float recentBonus = 0.10f;        // the next line is most likely said by someone already talking
constexpr size_t recentLines = 6, history = 200;

std::vector<float> scaledToPeak(const std::vector<float>& wave, float level) {
    float peak = 0.0f;
    for (const float s : wave) peak = std::max(peak, std::abs(s));
    std::vector<float> out(wave);
    if (peak > 1e-6f) for (auto& s : out) s *= level / peak;
    return out;
}

float dot(const std::vector<float>& a, const std::vector<float>& b) {
    return std::inner_product(a.begin(), a.end(), b.begin(), 0.0f);
}

void normalise(std::vector<float>& v) {
    const float length = std::sqrt(dot(v, v));
    if (length > 1e-9f) for (auto& x : v) x /= length;
}

} // namespace

// --- speech detection ----------------------------------------------------------------

SpeechDetector::SpeechDetector(const OrtRuntime& runtime, const std::filesystem::path& sileroModel)
    : model_(std::make_unique<OrtModel>(runtime, sileroModel, 1)) {
    const auto& names = model_->inputNames();
    v5_ = std::find(names.begin(), names.end(), "state") != names.end();
}

SpeechDetector::~SpeechDetector() = default;

std::vector<float> SpeechDetector::speech(const std::vector<float>& wave16k, bool brief) const {
    if (wave16k.size() < size_t(rate / 5)) return {};
    // The game may be quiet in the mix, and the detector misses quiet speech: judge it at a normal level.
    const auto wave = scaledToPeak(wave16k, 0.7f);
    float peak = 0.0f;
    for (const float s : wave16k) peak = std::max(peak, std::abs(s));
    if (peak < 1e-4f) return {}; // digital silence

    const size_t windows = wave.size() / window;
    std::vector<float> probability(windows, 0.0f);
    std::vector<float> state(v5_ ? 2 * 128 : 0, 0.0f), h(v5_ ? 0 : 2 * 64, 0.0f), c(v5_ ? 0 : 2 * 64, 0.0f);
    std::vector<float> context(64, 0.0f);
    const auto sr = Tensor::of(std::vector<int64_t>{}, std::vector<int64_t>{rate});
    for (size_t w = 0; w < windows; ++w) {
        const float* chunk = wave.data() + w * window;
        if (v5_) {
            std::vector<float> input(context);
            input.insert(input.end(), chunk, chunk + window);
            std::copy(chunk + window - 64, chunk + window, context.begin());
            const auto x = Tensor::of({1, int64_t(input.size())}, std::move(input));
            const auto s = Tensor::of({2, 1, 128}, state);
            auto out = model_->runMany({{"input", &x}, {"state", &s}, {"sr", &sr}}, {"output", "stateN"});
            probability[w] = out[0].floats.empty() ? 0.0f : out[0].floats[0];
            state = std::move(out[1].floats);
        } else {
            const auto x = Tensor::of({1, window}, std::vector<float>(chunk, chunk + window));
            const auto hs = Tensor::of({2, 1, 64}, h), cs = Tensor::of({2, 1, 64}, c);
            auto out = model_->runMany({{"x", &x}, {"h", &hs}, {"c", &cs}}, {"prob", "new_h", "new_c"});
            probability[w] = out[0].floats.empty() ? 0.0f : out[0].floats[0];
            h = std::move(out[1].floats);
            c = std::move(out[2].floats);
        }
    }

    // Spans with hysteresis, as Silero's own get_speech_timestamps cuts them.
    const size_t minSpeech = brief ? 3 : 8, minSilence = 3, pad = brief ? 0 : 480;
    std::vector<std::pair<size_t, size_t>> spans; // in windows
    bool inSpeech = false;
    size_t start = 0, quietSince = 0;
    for (size_t w = 0; w < windows; ++w) {
        const float p = probability[w];
        if (!inSpeech && p >= speechStarts) { inSpeech = true; start = w; quietSince = 0; }
        else if (inSpeech && p < speechEnds) {
            if (!quietSince) quietSince = w;
            if (w - quietSince + 1 >= minSilence) {
                if (quietSince - start >= minSpeech) spans.emplace_back(start, quietSince);
                inSpeech = false;
            }
        } else if (inSpeech && p >= speechEnds) {
            quietSince = 0;
        }
    }
    if (inSpeech && windows - start >= minSpeech) spans.emplace_back(start, windows);

    std::vector<float> out;
    for (const auto& [from, to] : spans) {
        const size_t a = from * window > pad ? from * window - pad : 0;
        const size_t b = std::min(wave16k.size(), to * window + pad);
        out.insert(out.end(), wave16k.begin() + std::ptrdiff_t(a), wave16k.begin() + std::ptrdiff_t(b));
    }
    return out;
}

// --- a man or a woman ------------------------------------------------------------------

GenderModel::GenderModel(const OrtRuntime& runtime, const std::filesystem::path& folder, int threads) {
    // The wiring of the network; its weights stay in the author's model.safetensors beside it.
    const auto graph = folder / "veyra-gender-graph.onnx";
    std::error_code error;
    if (std::filesystem::file_size(graph, error) != sizeof(genderGraph)) {
        std::ofstream out(graph, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(genderGraph), sizeof(genderGraph));
        if (!out) throw OrtError("cannot write " + graph.string());
    }
    model_ = std::make_unique<OrtModel>(runtime, graph, threads);
}

GenderModel::~GenderModel() = default;

std::optional<float> GenderModel::score(const std::vector<float>& speech16k) const {
    if (speech16k.size() < size_t(rate / 4)) return std::nullopt;
    auto wave = scaledToPeak(std::vector<float>(speech16k.begin(),
        speech16k.begin() + std::ptrdiff_t(std::min(speech16k.size(), size_t(4 * rate)))), 0.7f);
    const auto features = logMel(wave);
    if (features.frames < 10) return std::nullopt;
    const auto input = Tensor::of({1, 80, int64_t(features.frames)}, features.values);
    const auto logits = model_->run({{"feats", &input}}, "logits");
    if (logits.floats.size() < 2) return std::nullopt;
    return logits.floats[1] - logits.floats[0];
}

// --- voice fingerprints ----------------------------------------------------------------

VoicePrint::VoicePrint(const OrtRuntime& runtime, const std::filesystem::path& model, int threads)
    : model_(std::make_unique<OrtModel>(runtime, model, threads)) {}

VoicePrint::~VoicePrint() = default;

std::vector<float> VoicePrint::embed(const std::vector<float>& speech16k) const {
    auto wave = scaledToPeak(speech16k, 0.75f);
    if (wave.empty()) return {};
    // The model needs a few dozen frames: a very short clip is repeated.
    const auto shortest = size_t(6400);
    if (wave.size() < shortest) {
        const auto once = wave;
        while (wave.size() < shortest) wave.insert(wave.end(), once.begin(), once.end());
    }
    const auto features = kaldiFbank(wave);
    const auto input = Tensor::of({1, int64_t(features.frames), 80}, features.values);
    auto embedding = model_->run({{"feats", &input}}, "embs").floats;
    normalise(embedding);
    return embedding;
}

// --- the voices of a session -------------------------------------------------------------

void VoiceGroups::clear() {
    lines_.clear();
    centroids_.clear();
    recent_.clear();
}

VoiceGroups::Match VoiceGroups::match(const std::vector<float>& embedding) const {
    Match best;
    float bestScore = -2.0f;
    for (const auto& [name, centre] : centroids_) {
        float score = dot(centre, embedding);
        if (std::find(recent_.begin(), recent_.end(), name) != recent_.end()) score += recentBonus;
        if (score > bestScore) { bestScore = score; best.closest = name; }
    }
    best.likeness = bestScore < -1.0f ? 0.0f : bestScore;
    if (bestScore >= matchAbove) best.voice = best.closest;
    return best;
}

std::optional<bool> VoiceGroups::female(const std::string& voice) const {
    int women = 0, votes = 0;
    for (const auto& line : lines_)
        if (line.voice == voice && line.female) { ++votes; women += *line.female ? 1 : 0; }
    if (!votes) return std::nullopt;
    return women * 2 > votes;
}

VoiceGroups::Credited VoiceGroups::assign(const std::vector<float>& embedding, std::optional<bool> isFemale) {
    std::vector<std::string> before;
    for (const auto& c : centroids_) before.push_back(c.first);
    lines_.push_back({embedding, {}, isFemale});
    if (lines_.size() > history) lines_.erase(lines_.begin());
    const size_t n = lines_.size();

    // Average-linkage grouping of every line, cut where groups are less alike than joinAbove.
    std::vector<std::vector<size_t>> groups(n);
    for (size_t i = 0; i < n; ++i) groups[i] = {i};
    std::vector<std::vector<float>> distance(n, std::vector<float>(n, 0.0f));
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j)
            distance[i][j] = distance[j][i] = 1.0f - dot(lines_[i].embedding, lines_[j].embedding);
    std::vector<bool> alive(n, true);
    for (;;) {
        float closest = 2.0f;
        size_t a = 0, b = 0;
        for (size_t i = 0; i < n; ++i) {
            if (!alive[i]) continue;
            for (size_t j = i + 1; j < n; ++j)
                if (alive[j] && distance[i][j] < closest) { closest = distance[i][j]; a = i; b = j; }
        }
        if (closest > 1.0f - joinAbove) break;
        const float na = float(groups[a].size()), nb = float(groups[b].size());
        for (size_t k = 0; k < n; ++k) {
            if (!alive[k] || k == a || k == b) continue;
            distance[a][k] = distance[k][a] = (na * distance[a][k] + nb * distance[b][k]) / (na + nb);
        }
        groups[a].insert(groups[a].end(), groups[b].begin(), groups[b].end());
        groups[b].clear();
        alive[b] = false;
    }

    // A name goes to the group holding most of its lines - the biggest claims first.
    struct Claim { size_t count; size_t group; std::string name; };
    std::vector<Claim> claims;
    for (size_t g = 0; g < n; ++g) {
        if (!alive[g]) continue;
        std::map<std::string, size_t> counts;
        for (const size_t line : groups[g]) if (!lines_[line].voice.empty()) ++counts[lines_[line].voice];
        for (const auto& [name, count] : counts) claims.push_back({count, g, name});
    }
    std::stable_sort(claims.begin(), claims.end(), [](const Claim& x, const Claim& y) { return x.count > y.count; });
    std::map<size_t, std::string> named;
    for (const auto& claim : claims) {
        if (named.count(claim.group)) continue;
        if (std::any_of(named.begin(), named.end(), [&](const auto& entry) { return entry.second == claim.name; })) continue;
        named[claim.group] = claim.name;
    }
    Credited result;
    std::vector<std::pair<std::string, size_t>> born;
    for (size_t g = 0; g < n; ++g) {
        if (!alive[g] || named.count(g)) continue;
        named[g] = "Голос " + std::to_string(++count_);
        born.emplace_back(named[g], g);
    }
    centroids_.clear();
    for (const auto& [g, name] : named) {
        std::vector<float> centre(lines_[0].embedding.size(), 0.0f);
        for (const size_t line : groups[g]) {
            lines_[line].voice = name;
            for (size_t k = 0; k < centre.size(); ++k) centre[k] += lines_[line].embedding[k];
        }
        normalise(centre);
        centroids_.emplace_back(name, std::move(centre));
    }
    result.voice = lines_.back().voice;
    for (const auto& [name, g] : born) {
        if (name == result.voice) result.isNew = true;
        else result.born.emplace_back(name, female(name));
    }
    for (const auto& name : before)
        if (std::none_of(centroids_.begin(), centroids_.end(), [&](const auto& c) { return c.first == name; }))
            result.gone.push_back(name);
    recent_.push_back(result.voice);
    if (recent_.size() > recentLines) recent_.erase(recent_.begin());
    return result;
}

// --- all of it -------------------------------------------------------------------------

VoiceListener::VoiceListener(const OrtRuntime& runtime, const std::filesystem::path& silero,
                             const std::filesystem::path& genderFolder, const std::filesystem::path& speakerModel,
                             int threads, bool recognise)
    : detector_(runtime, silero), gender_(runtime, genderFolder, threads) {
    if (recognise) print_ = std::make_unique<VoicePrint>(runtime, speakerModel, threads);
}

VoiceListener::~VoiceListener() = default;

float loudnessDb(const std::vector<float>& wave) {
    if (wave.empty()) return -120.0f;
    double energy = 0.0;
    for (const float s : wave) energy += double(s) * s;
    return float(10.0 * std::log10(std::max(1e-12, energy / double(wave.size()))));
}

float arousalOf(float louderDb, float higherSemitones) {
    // Shouting is some 6-10 dB above talk and an excited voice 3-6 semitones above its usual;
    // a few dB or a semitone or two either way is only how the game mixes one line against
    // another, or what a short opening lets the pitch be measured at.
    const auto beyond = [](float x, float deadZone) {
        return x > deadZone ? x - deadZone : x < -deadZone ? x + deadZone : 0.0f;
    };
    // (The pitch of a short opening in a game's mix is often off by a fifth or so, so it counts
    // for less than loudness, and never alone for more than half.)
    const float byPitch = std::clamp(beyond(higherSemitones, 3.0f) / 8.0f, -0.5f, 0.5f);
    return std::clamp(beyond(louderDb, 2.5f) / 8.0f + byPitch, -1.0f, 1.0f);
}

void VoiceListener::reset() {
    groups_.clear();
    for (auto& usual : usualLoudness_) usual.reset();
    for (auto& usual : usualPitch_) usual.reset();
}

std::optional<float> VoiceListener::arousal(const std::vector<float>& speech, std::optional<bool> female,
                                            std::optional<float> pitch) const {
    if (speech.size() < size_t(0.3 * rate)) return std::nullopt;
    const float loud = loudnessDb(speech);
    if (loud < -70.0f) return std::nullopt;
    // Against the usual of the same sex: men and women are often mixed at different levels.
    const int sex = female ? (*female ? 1 : 0) : -1;
    // (The first line of a sex is its own usual: the session's whole usual may be another level.)
    const auto& usualLoud = sex >= 0 ? usualLoudness_[sex] : usualLoudness_[2];
    const float louder = usualLoud ? loud - *usualLoud : 0.0f;
    float higher = 0.0f;
    if (pitch && sex >= 0 && usualPitch_[sex]) higher = 12.0f * std::log2(*pitch / *usualPitch_[sex]);
    return arousalOf(louder, higher);
}

// A whole line heard: the usual of the session moves towards it - slowly, so that one shout
// does not make the next shout ordinary.
void VoiceListener::learnUsual(const std::vector<float>& speech, std::optional<bool> female) {
    const float loud = loudnessDb(speech);
    if (loud < -70.0f) return;
    const auto learn = [](std::optional<float>& usual, float value) {
        usual = usual ? 0.85f * *usual + 0.15f * value : value;
    };
    learn(usualLoudness_[2], loud);
    if (female) learn(usualLoudness_[*female ? 1 : 0], loud);
    const auto pitch = medianPitch(speech, rate);
    if (!female || !pitch) return;
    auto& usual = usualPitch_[*female ? 1 : 0];
    usual = usual ? 0.85f * *usual + 0.15f * *pitch : *pitch;
}

VoiceListener::Heard VoiceListener::opening(const std::vector<float>& wave16k) {
    Heard heard;
    auto speech = detector_.speech(wave16k, true);
    heard.speechSeconds = float(speech.size()) / rate;
    if (speech.size() < size_t(0.3 * rate)) return heard; // nothing to go by
    heard.score = gender_.score(speech);
    if (heard.score) heard.female = *heard.score > 0.0f;
    heard.pitch = medianPitch(speech, rate);
    heard.arousal = arousal(speech, heard.female, heard.pitch);
    if (print_) {
        speech.resize(std::min(speech.size(), size_t(rate))); // the opening second is enough to pick
        heard.voice = groupsMatch(speech);
    }
    return heard;
}

std::string VoiceListener::groupsMatch(const std::vector<float>& speech) {
    return groups_.match(print_->embed(speech)).voice;
}

VoiceGroups::Credited VoiceListener::lineEnded(const std::vector<float>& wave16k, std::optional<bool>& female,
                                                float* speechSeconds) {
    female.reset();
    auto speech = detector_.speech(wave16k, false);
    if (speechSeconds) *speechSeconds = float(speech.size()) / rate;
    if (speech.size() < size_t(1.0 * rate)) return {}; // too little to tell whose voice it was
    if (const auto score = gender_.score(speech)) female = *score > 0.0f;
    learnUsual(speech, female);
    if (!print_) return {};
    speech.resize(std::min(speech.size(), size_t(12 * rate)));
    return groups_.assign(print_->embed(speech), female);
}

} // namespace veyra::tts
