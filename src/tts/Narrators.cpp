// SPDX-License-Identifier: GPL-3.0-only
#include "Narrators.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace veyra::tts {
namespace {

struct Base { const char* style; float pitch; bool female; bool byHandOnly; const char* name; const char* low; const char* high; };

// Typical pitch of each TeraTTSv2 voice speaking Russian (measured). The first man's and woman's
// voices are the ones that speak when nobody chose: the user, having listened, preferred
// eng_m4 and eng_f5 to the model authors' ru_m5 and ru_f1. eng_m3 is a man pitched between the
// sexes: offered, never handed out on its own.
constexpr Base bases[]{
    {"eng_m4", 103, false, false, "Виктор", "Фёдор", "Роман"},
    {"ru_m5", 123, false, false, "Артём", "Борис", "Денис"},
    {"ru_m1", 145, false, false, "Максим", "Глеб", "Кирилл"},
    {"eng_f5", 267, true, false, "Диана", "Инна", "Мила"},
    {"ru_f1", 286, true, false, "Алиса", "Ольга", "Соня"},
    {"ru_f2", 235, true, false, "Вера", "Тамара", "Лиза"},
    {"eng_f3", 211, true, false, "Марина", "Нина", "Катя"},
    {"eng_m3", 186, false, true, "Саша", nullptr, nullptr},
};
// 8 % either way: a variant is then about as unlike its own narrator as two narrators are unlike
// each other (measured with the speaker-fingerprint model), and still sounds like a person.
constexpr float lowRatio = 0.92f, highRatio = 1.08f;

std::vector<Narrator> build() {
    std::vector<Narrator> out;
    for (const auto& b : bases) out.push_back({b.style, b.style, 1.0f, b.female, b.byHandOnly, b.pitch, b.name});
    for (const auto& [suffix, ratio] : {std::pair{"_low", lowRatio}, std::pair{"_high", highRatio}})
        for (const auto& b : bases) {
            if (b.byHandOnly) continue;
            out.push_back({std::string(b.style) + suffix, b.style, ratio, b.female, false, b.pitch * ratio,
                           ratio < 1.0f ? b.low : b.high});
        }
    return out;
}

} // namespace

const std::vector<Narrator>& narrators() {
    static const auto all = build();
    return all;
}

const Narrator* findNarrator(std::string_view id) {
    for (const auto& n : narrators()) if (n.id == id) return &n;
    return nullptr;
}

const Narrator& defaultNarrator(bool female) {
    return *findNarrator(female ? "eng_f5" : "eng_m4");
}

std::string pickNarrator(bool female, const std::vector<std::string>& taken, std::optional<float> pitch) {
    std::vector<const Narrator*> pool, free;
    for (const auto& n : narrators())
        if (n.female == female && !n.byHandOnly) pool.push_back(&n);
    for (const auto* n : pool)
        if (std::find(taken.begin(), taken.end(), n->id) == taken.end()) free.push_back(n);
    if (free.empty()) { // more speakers than narrators: start sharing, in turn
        const auto inPool = std::count_if(taken.begin(), taken.end(), [&](const std::string& id) {
            const auto* n = findNarrator(id);
            return n && n->female == female && !n->byHandOnly;
        });
        return pool[size_t(inPool) % pool.size()]->id;
    }
    // Variants of a narrator already speaking count against a choice: they sound related.
    auto kin = [&](const Narrator& n) {
        return float(std::count_if(taken.begin(), taken.end(), [&](const std::string& id) {
            const auto* other = findNarrator(id);
            return other && other->style == n.style;
        }));
    };
    const Narrator* best = free.front();
    float bestScore = std::numeric_limits<float>::max();
    for (const auto* n : free) {
        // (Each relative in use counts as 25 Hz further away: by pitch alone a scene of three men
        // was given three variants of one narrator.)
        const float score = pitch ? std::abs(n->pitch - *pitch) + 25.0f * kin(*n) : kin(*n);
        if (score < bestScore) { bestScore = score; best = n; }
    }
    return best->id;
}

std::optional<float> medianPitch(const std::vector<float>& wave, int rate) {
    if (rate <= 0) return std::nullopt;
    const int frame = rate / 25, hop = rate / 50;            // 40 ms windows, 20 ms apart
    const int lowLag = rate / 400, highLag = rate / 60;       // 400 Hz .. 60 Hz
    if (int(wave.size()) < frame * 4 || highLag >= frame) return std::nullopt;
    std::vector<std::vector<float>> frames;
    std::vector<double> energy;
    for (size_t start = 0; start + size_t(frame) <= wave.size(); start += size_t(hop)) {
        std::vector<float> f(wave.begin() + std::ptrdiff_t(start), wave.begin() + std::ptrdiff_t(start + size_t(frame)));
        double mean = 0;
        for (const float s : f) mean += s;
        mean /= frame;
        double e = 0;
        for (auto& s : f) { s = float(s - mean); e += double(s) * s; }
        frames.push_back(std::move(f));
        energy.push_back(e / frame);
    }
    const double loudest = *std::max_element(energy.begin(), energy.end());
    std::vector<float> pitches;
    for (size_t k = 0; k < frames.size(); ++k) {
        if (energy[k] <= 0.2 * loudest || energy[k] <= 0) continue;
        const auto& f = frames[k];
        double zero = 0;
        for (const float s : f) zero += double(s) * s;
        int bestLag = 0;
        double bestValue = -1;
        for (int lag = lowLag; lag < highLag; ++lag) {
            double c = 0;
            for (int i = 0; i + lag < frame; ++i) c += double(f[size_t(i)]) * f[size_t(i + lag)];
            if (c > bestValue) { bestValue = c; bestLag = lag; }
        }
        if (bestLag > 0 && bestValue / zero > 0.4) pitches.push_back(float(rate) / float(bestLag));
    }
    if (pitches.size() < 3) return std::nullopt;
    std::nth_element(pitches.begin(), pitches.begin() + std::ptrdiff_t(pitches.size() / 2), pitches.end());
    return pitches[pitches.size() / 2];
}

std::vector<float> playFaster(const std::vector<float>& wave, float ratio) {
    if (wave.empty() || !(ratio > 0.0f) || ratio == 1.0f) return wave;
    // Windowed-sinc (Lanczos, 8 lobes) interpolation; the ratios used are near 1, so no
    // anti-aliasing beyond the kernel is needed for speech.
    constexpr int lobes = 8;
    const size_t out = size_t(std::floor(double(wave.size()) / ratio));
    std::vector<float> result(out);
    const double cutoff = std::min(1.0, 1.0 / ratio);
    for (size_t j = 0; j < out; ++j) {
        const double at = double(j) * ratio;
        const auto centre = int64_t(std::floor(at));
        double sum = 0, weights = 0;
        for (int64_t k = centre - lobes + 1; k <= centre + lobes; ++k) {
            if (k < 0 || k >= int64_t(wave.size())) continue;
            const double x = (at - double(k)) * cutoff;
            const double sinc = x == 0 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
            const double window = std::abs(x) >= lobes ? 0.0
                : (x == 0 ? 1.0 : std::sin(std::numbers::pi * x / lobes) / (std::numbers::pi * x / lobes));
            const double w = sinc * window;
            sum += w * wave[size_t(k)];
            weights += w;
        }
        result[j] = float(weights != 0 ? sum / weights : 0.0);
    }
    return result;
}

} // namespace veyra::tts
