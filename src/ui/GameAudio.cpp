// SPDX-License-Identifier: GPL-3.0-only
#include "GameAudio.h"
#include "AsrModels.h"
#include "veyra/sink/RecordingAudioTap.h"

#include <algorithm>

namespace veyra::ui {
namespace {
constexpr qint64 rate = asr::sampleRate; // 16 kHz
constexpr size_t keptSamples = size_t(rate) * 30;
constexpr qint64 gapUs = 150000;   // a pause in the PCM longer than this is filled with silence
constexpr qint64 resetUs = 3000000; // ...and one longer than this starts the buffer afresh
}

GameAudio::GameAudio() {
    timer_.setInterval(40); // the tap holds about 1.3 s of blocks
    connect(&timer_, &QTimer::timeout, this, &GameAudio::pump);
}

GameAudio::~GameAudio() { stop(); }

void GameAudio::start() {
    if (tap_) return;
    tap_ = std::make_shared<asr::AudioTap>();
    sink::addRecordingAudioSink(tap_);
    timer_.start();
}

void GameAudio::stop() {
    if (!tap_) return;
    timer_.stop();
    sink::removeRecordingAudioSink(tap_.get());
    tap_.reset();
    resampler_.reset();
    samples_.clear();
    endUs_ = 0;
    renderer_ = nullptr;
}

void GameAudio::pump() {
    if (!tap_) return;
    tap_->takeDrops();
    asr::AudioTap::Block block;
    while (tap_->pop(block)) {
        if (renderer_ != block.renderer) {
            resampler_.reset();
            renderer_ = block.renderer;
        }
        const qint64 blockEnd = block.playoutUs + qint64(block.frames) * 1000000 / asr::inputRate;
        if (endUs_) {
            const qint64 gap = block.playoutUs - endUs_;
            if (gap > resetUs || gap < -resetUs) {
                samples_.clear();
                resampler_.reset();
            } else if (gap > gapUs) {
                samples_.insert(samples_.end(), size_t(gap * rate / 1000000), 0.0f);
            }
        }
        if (!resampler_.convert(block, converted_)) {
            resampler_.reset();
            continue;
        }
        samples_.insert(samples_.end(), converted_.begin(), converted_.end());
        endUs_ = blockEnd;
        if (samples_.size() > keptSamples) samples_.erase(samples_.begin(), samples_.end() - std::ptrdiff_t(keptSamples));
    }
}

std::vector<float> GameAudio::clip(qint64 fromUs, qint64 toUs) const {
    if (!endUs_ || toUs <= fromUs || samples_.empty()) return {};
    const auto index = [&](qint64 us) {
        const qint64 back = (endUs_ - us) * rate / 1000000;
        return std::clamp<qint64>(qint64(samples_.size()) - back, 0, qint64(samples_.size()));
    };
    const auto from = index(fromUs), to = index(toUs);
    if (to <= from) return {};
    return std::vector<float>(samples_.begin() + std::ptrdiff_t(from), samples_.begin() + std::ptrdiff_t(to));
}

}
