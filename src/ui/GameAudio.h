// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// The last half minute of what the player plays, as 16 kHz mono with the time it was heard:
// the voice listens to it to tell who speaks a subtitle line. Taken from the same tap as
// speech recognition (before volume, mute and the voice's own ducking, so the original is
// always at full level and the voice itself, played elsewhere, is never in it).
#include "AsrAudioTap.h"

#include <QTimer>
#include <deque>
#include <memory>
#include <vector>

namespace veyra::ui {

class GameAudio final : public QObject {
    Q_OBJECT
public:
    GameAudio();
    ~GameAudio() override;
    void start();
    void stop();
    bool running() const { return bool(tap_); }
    // What was heard between two moments of sink::recordingClockUs(); empty if none of it is kept.
    std::vector<float> clip(qint64 fromUs, qint64 toUs) const;
    qint64 endUs() const { return endUs_; } // the moment the newest kept sample was heard

private:
    void pump();

    std::shared_ptr<asr::AudioTap> tap_;
    asr::Resampler resampler_;
    QTimer timer_;
    std::deque<float> samples_;
    std::vector<float> converted_;
    qint64 endUs_ = 0;
    const void* renderer_ = nullptr;
};

}
