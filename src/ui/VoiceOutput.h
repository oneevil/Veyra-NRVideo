// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Where live translation's voice is heard: a WASAPI shared-mode stream of its own on the
// output Veyra plays to (Settings -> audio output, or the Windows default), so that the voice
// never enters the player's renderer - whose PCM feeds speech recognition and recordings.
// While it speaks, the player is turned down (sink::setVoiceDuck) and brought back after.
//
// Two lanes are mixed: the main one, where lines follow one another, and a second one for a
// line said over it - another person talking at the same time, as in the original.
#include <atomic>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>

namespace veyra::ui {

class VoiceOutput {
public:
    static constexpr int rate = 44100; // mono float PCM, as the voice makes it
    VoiceOutput();
    ~VoiceOutput();
    VoiceOutput(const VoiceOutput&) = delete;
    VoiceOutput& operator=(const VoiceOutput&) = delete;

    void start();
    void stop();
    static constexpr int lanes = 2;
    void push(const float* samples, size_t count, int lane = 0);
    void clear(int lane = -1); // -1: every lane
    double bufferedSeconds(int lane = 0) const;
    void setVolume(float value) { volume_.store(value); }
    // How far the player is turned down while the voice speaks (1 = not at all).
    void setDuck(float value) { duck_.store(value); }
    bool failed() const { return failed_.load(); }

private:
    void run();
    bool take(float* out, size_t count);
    bool anyBuffered() const;

    mutable std::mutex mutex_;
    std::deque<float> pcm_[lanes];
    std::thread thread_;
    std::atomic<bool> running_{false}, failed_{false};
    std::atomic<float> volume_{1.0f}, duck_{0.35f};
    void* wake_ = nullptr; // event that stops the thread
};

}
