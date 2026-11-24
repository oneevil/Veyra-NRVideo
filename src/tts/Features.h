// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Spectral features of 16 kHz mono speech for the voice networks, computed as their own
// training front ends did (checked against torchaudio sample for sample):
//  * kaldiFbank: Kaldi-style 80-bin log filterbank, 25 ms / 10 ms, Hamming window, no dither,
//    mean-normalised over time - what WeSpeaker's speaker models take (frames x 80);
//  * logMel: torchaudio MelSpectrogram (n_fft 512, Hamming 400, hop 160, 20-7600 Hz, HTK mel)
//    of the pre-emphasised signal, log, mean-normalised - what the ECAPA gender model takes
//    (80 x frames).
#include <cstddef>
#include <vector>

namespace veyra::tts {

struct Features {
    size_t frames = 0;
    std::vector<float> values;
};

Features kaldiFbank(const std::vector<float>& wave16k); // [frames][80]; `wave16k` in -1..1
Features logMel(const std::vector<float>& wave16k);     // [80][frames]

} // namespace veyra::tts
