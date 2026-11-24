// SPDX-License-Identifier: GPL-3.0-only
#include "Features.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>

namespace veyra::tts {
namespace {

constexpr int fftSize = 512;
constexpr int bins = 80;
constexpr int frameLength = 400, frameShift = 160;
constexpr double pi = 3.14159265358979323846;

// Power spectrum |X(k)|^2, k = 0..256, of 512 real samples (iterative radix-2 FFT).
void powerSpectrum(const double* frame, double* power) {
    static const auto twiddles = [] {
        std::vector<std::complex<double>> w(fftSize / 2);
        for (int k = 0; k < fftSize / 2; ++k) w[size_t(k)] = std::polar(1.0, -2.0 * pi * k / fftSize);
        return w;
    }();
    std::complex<double> data[fftSize];
    for (int i = 0, j = 0; i < fftSize; ++i) {
        data[j] = frame[i];
        int bit = fftSize >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j |= bit;
    }
    for (int length = 2; length <= fftSize; length <<= 1) {
        const int step = fftSize / length;
        for (int start = 0; start < fftSize; start += length)
            for (int k = 0; k < length / 2; ++k) {
                const auto t = twiddles[size_t(k * step)] * data[start + k + length / 2];
                data[start + k + length / 2] = data[start + k] - t;
                data[start + k] += t;
            }
    }
    for (int k = 0; k <= fftSize / 2; ++k) power[k] = std::norm(data[k]);
}

// Kaldi's mel banks: 1127 ln(1 + f / 700), triangles in the mel domain, 20 Hz to Nyquist,
// over the 256 FFT bins below Nyquist.
const std::vector<double>& kaldiBanks() {
    static const auto banks = [] {
        auto mel = [](double f) { return 1127.0 * std::log(1.0 + f / 700.0); };
        const double low = mel(20.0), high = mel(8000.0), delta = (high - low) / (bins + 1);
        std::vector<double> w(size_t(bins) * (fftSize / 2 + 1), 0.0);
        for (int b = 0; b < bins; ++b) {
            const double left = low + b * delta, center = left + delta, right = center + delta;
            for (int k = 0; k < fftSize / 2; ++k) {
                const double m = mel(16000.0 * k / fftSize);
                if (m > left && m < right)
                    w[size_t(b) * (fftSize / 2 + 1) + size_t(k)] = m <= center ? (m - left) / (center - left)
                                                                               : (right - m) / (right - center);
            }
        }
        return w;
    }();
    return banks;
}

// torchaudio.functional.melscale_fbanks(257, 20, 7600, 80, 16000, norm=None, mel_scale="htk").
const std::vector<double>& htkBanks() {
    static const auto banks = [] {
        auto mel = [](double f) { return 2595.0 * std::log10(1.0 + f / 700.0); };
        auto hz = [](double m) { return 700.0 * (std::pow(10.0, m / 2595.0) - 1.0); };
        const double low = mel(20.0), high = mel(7600.0);
        std::vector<double> points(bins + 2);
        for (int i = 0; i < bins + 2; ++i) points[size_t(i)] = hz(low + (high - low) * i / (bins + 1));
        std::vector<double> w(size_t(bins) * (fftSize / 2 + 1), 0.0);
        for (int k = 0; k <= fftSize / 2; ++k) {
            const double f = 8000.0 * k / (fftSize / 2);
            for (int b = 0; b < bins; ++b) {
                const double down = (f - points[size_t(b)]) / (points[size_t(b) + 1] - points[size_t(b)]);
                const double up = (points[size_t(b) + 2] - f) / (points[size_t(b) + 2] - points[size_t(b) + 1]);
                w[size_t(b) * (fftSize / 2 + 1) + size_t(k)] = std::max(0.0, std::min(down, up));
            }
        }
        return w;
    }();
    return banks;
}

void subtractMean(std::vector<float>& values, size_t frames, bool frameMajor) {
    for (int b = 0; b < bins; ++b) {
        double sum = 0.0;
        for (size_t t = 0; t < frames; ++t) sum += values[frameMajor ? t * bins + size_t(b) : size_t(b) * frames + t];
        const float mean = float(sum / double(std::max<size_t>(1, frames)));
        for (size_t t = 0; t < frames; ++t) values[frameMajor ? t * bins + size_t(b) : size_t(b) * frames + t] -= mean;
    }
}

} // namespace

Features kaldiFbank(const std::vector<float>& wave) {
    Features out;
    if (wave.size() < size_t(frameLength)) return out;
    out.frames = 1 + (wave.size() - frameLength) / frameShift; // snip_edges
    out.values.resize(out.frames * bins);
    static const auto window = [] {
        std::vector<double> w(frameLength);
        for (int i = 0; i < frameLength; ++i) w[size_t(i)] = 0.54 - 0.46 * std::cos(2.0 * pi * i / (frameLength - 1));
        return w;
    }();
    const auto& banks = kaldiBanks();
    double frame[fftSize], power[fftSize / 2 + 1];
    const double epsilon = std::numeric_limits<float>::epsilon();
    for (size_t t = 0; t < out.frames; ++t) {
        double mean = 0.0;
        for (int i = 0; i < frameLength; ++i) {
            frame[i] = double(wave[t * frameShift + size_t(i)]) * 32768.0;
            mean += frame[i];
        }
        mean /= frameLength;
        for (int i = 0; i < frameLength; ++i) frame[i] -= mean;          // remove DC offset
        for (int i = frameLength - 1; i > 0; --i) frame[i] -= 0.97 * frame[i - 1]; // pre-emphasis
        frame[0] -= 0.97 * frame[0];
        for (int i = 0; i < frameLength; ++i) frame[i] *= window[size_t(i)];
        std::fill(frame + frameLength, frame + fftSize, 0.0);
        powerSpectrum(frame, power);
        for (int b = 0; b < bins; ++b) {
            double energy = 0.0;
            const double* w = banks.data() + size_t(b) * (fftSize / 2 + 1);
            for (int k = 0; k <= fftSize / 2; ++k) energy += w[k] * power[k];
            out.values[t * bins + size_t(b)] = float(std::log(std::max(energy, epsilon)));
        }
    }
    subtractMean(out.values, out.frames, true);
    return out;
}

Features logMel(const std::vector<float>& wave) {
    Features out;
    const size_t n = wave.size();
    if (n < 2) return out;
    // Pre-emphasis with the first sample reflected in front, as the model's front end does.
    std::vector<double> x(n);
    x[0] = double(wave[0]) - 0.97 * double(wave[1]);
    for (size_t i = 1; i < n; ++i) x[i] = double(wave[i]) - 0.97 * double(wave[i - 1]);
    // Centred frames: the signal reflected by half an FFT at both ends.
    const int half = fftSize / 2;
    if (n <= size_t(half)) return out;
    auto at = [&](long long i) -> double {
        if (i < 0) i = -i;
        if (i >= (long long)n) i = 2 * ((long long)n - 1) - i;
        return x[size_t(i)];
    };
    out.frames = 1 + n / frameShift;
    out.values.resize(size_t(bins) * out.frames);
    static const auto window = [] { // periodic Hamming of 400, centred in 512
        std::vector<double> w(fftSize, 0.0);
        const int offset = (fftSize - frameLength) / 2;
        for (int i = 0; i < frameLength; ++i)
            w[size_t(offset + i)] = 0.54 - 0.46 * std::cos(2.0 * pi * i / frameLength);
        return w;
    }();
    const auto& banks = htkBanks();
    double frame[fftSize], power[fftSize / 2 + 1];
    for (size_t t = 0; t < out.frames; ++t) {
        const long long start = (long long)(t * frameShift) - half;
        for (int i = 0; i < fftSize; ++i) frame[i] = at(start + i) * window[size_t(i)];
        powerSpectrum(frame, power);
        for (int b = 0; b < bins; ++b) {
            double energy = 0.0;
            const double* w = banks.data() + size_t(b) * (fftSize / 2 + 1);
            for (int k = 0; k <= fftSize / 2; ++k) energy += w[k] * power[k];
            out.values[size_t(b) * out.frames + t] = float(std::log(energy + 1e-6));
        }
    }
    subtractMean(out.values, out.frames, false);
    return out;
}

} // namespace veyra::tts
