// SPDX-License-Identifier: GPL-3.0-only
#include "VoiceOutput.h"
#include "veyra/Log.h"
#include "veyra/sink/WasapiAudioSink.h"

#include <windows.h>
#include <objbase.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

#include <algorithm>
#include <iterator>
#include <chrono>
#include <cmath>
#include <format>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace veyra::ui {
namespace {

constexpr size_t maxBufferedSamples = size_t(VoiceOutput::rate) * 90; // a bound, never reached in use
constexpr double duckDownMs = 120.0, duckUpMs = 450.0;   // how fast the player goes down and comes back
constexpr double holdMs = 350.0;                          // the player stays down this long after a line
constexpr float overlapGain = 0.8f;                       // the second lane against the main one

struct Stream {
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    UINT32 frames = 0;
    uint64_t generation = 0;
};

bool open(Stream& stream, HANDLE event) {
    ComPtr<IMMDeviceEnumerator> devices;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices)))) return false;
    stream.generation = sink::renderEndpointGeneration();
    const auto preferred = sink::preferredRenderEndpoint();
    if (preferred.empty() || FAILED(devices->GetDevice(preferred.c_str(), &stream.device)))
        if (FAILED(devices->GetDefaultAudioEndpoint(eRender, eConsole, &stream.device))) return false;
    if (FAILED(stream.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                       reinterpret_cast<void**>(stream.client.GetAddressOf())))) return false;
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = 1;
    format.nSamplesPerSec = VoiceOutput::rate;
    format.wBitsPerSample = 32;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = VoiceOutput::rate * 4;
    // Windows converts the voice's mono 44.1 kHz to the device's own format.
    const HRESULT hr = stream.client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        REFERENCE_TIME(400000), 0, &format, nullptr); // 40 ms
    if (FAILED(hr)) {
        log::warn("voice", std::format("output Initialize hr=0x{:08X}", unsigned(hr)));
        return false;
    }
    return SUCCEEDED(stream.client->SetEventHandle(event))
        && SUCCEEDED(stream.client->GetBufferSize(&stream.frames))
        && SUCCEEDED(stream.client->GetService(IID_PPV_ARGS(&stream.render)))
        && SUCCEEDED(stream.client->Start());
}

} // namespace

VoiceOutput::VoiceOutput() : wake_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

VoiceOutput::~VoiceOutput() {
    stop();
    if (wake_) CloseHandle(static_cast<HANDLE>(wake_));
}

void VoiceOutput::start() {
    if (running_.exchange(true)) return;
    failed_ = false;
    ResetEvent(static_cast<HANDLE>(wake_));
    thread_ = std::thread([this] { run(); });
}

void VoiceOutput::stop() {
    if (!running_.exchange(false)) return;
    SetEvent(static_cast<HANDLE>(wake_));
    if (thread_.joinable()) thread_.join();
    clear();
    sink::setVoiceDuck(1.0f);
}

void VoiceOutput::push(const float* samples, size_t count, int lane) {
    lane = std::clamp(lane, 0, lanes - 1);
    std::lock_guard lock(mutex_);
    auto& pcm = pcm_[lane];
    if (pcm.size() + count > maxBufferedSamples) return;
    pcm.insert(pcm.end(), samples, samples + count);
}

void VoiceOutput::clear(int lane) {
    std::lock_guard lock(mutex_);
    for (int i = 0; i < lanes; ++i)
        if (lane < 0 || lane == i) pcm_[i].clear();
}

double VoiceOutput::bufferedSeconds(int lane) const {
    lane = std::clamp(lane, 0, lanes - 1);
    std::lock_guard lock(mutex_);
    return double(pcm_[lane].size()) / rate;
}

bool VoiceOutput::anyBuffered() const {
    std::lock_guard lock(mutex_);
    return std::any_of(std::begin(pcm_), std::end(pcm_), [](const auto& pcm) { return !pcm.empty(); });
}

bool VoiceOutput::take(float* out, size_t count) {
    std::lock_guard lock(mutex_);
    std::fill(out, out + count, 0.0f);
    bool any = false;
    for (int lane = 0; lane < lanes; ++lane) {
        auto& pcm = pcm_[lane];
        const size_t have = std::min(count, pcm.size());
        // The line said over another is a little quieter, so that the main one stays clear.
        const float gain = lane == 0 ? 1.0f : overlapGain;
        for (size_t i = 0; i < have; ++i) out[i] += pcm[i] * gain;
        pcm.erase(pcm.begin(), pcm.begin() + std::ptrdiff_t(have));
        any = any || have > 0;
    }
    return any;
}

void VoiceOutput::run() {
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const HANDLE waits[2]{static_cast<HANDLE>(wake_), event};
    using clock = std::chrono::steady_clock;
    auto lastSound = clock::time_point{};
    auto lastStep = clock::now();
    float duck = 1.0f;
    std::vector<float> block;
    int failures = 0;
    while (running_) {
        Stream stream;
        if (!open(stream, event)) {
            if (++failures == 5) {
                failed_ = true;
                log::warn("voice", "cannot open the audio output for the voice");
            }
            if (WaitForSingleObject(static_cast<HANDLE>(wake_), 1000) == WAIT_OBJECT_0) break;
            continue;
        }
        failures = 0;
        failed_ = false;
        log::info("voice", std::format("output opened buffer={} frames", stream.frames));
        while (running_) {
            const DWORD woke = WaitForMultipleObjects(2, waits, FALSE, 200);
            if (woke == WAIT_OBJECT_0) break;
            // The output chosen in Veyra changed: follow it, as the player's renderer does.
            if (stream.generation != sink::renderEndpointGeneration()) break;
            UINT32 padding = 0;
            if (FAILED(stream.client->GetCurrentPadding(&padding))) break; // device lost: reopen
            const UINT32 room = stream.frames - std::min(padding, stream.frames);
            if (room) {
                BYTE* data = nullptr;
                if (FAILED(stream.render->GetBuffer(room, &data))) break;
                block.resize(room);
                if (take(block.data(), room)) lastSound = clock::now();
                const float volume = std::clamp(volume_.load(), 0.0f, 2.0f);
                auto* out = reinterpret_cast<float*>(data);
                for (UINT32 i = 0; i < room; ++i) out[i] = std::clamp(block[i] * volume, -1.0f, 1.0f);
                if (FAILED(stream.render->ReleaseBuffer(room, 0))) break;
            }
            // The player goes down while the voice speaks and comes back a moment after.
            const auto now = clock::now();
            const double stepMs = std::chrono::duration<double, std::milli>(now - lastStep).count();
            lastStep = now;
            const bool speaking = anyBuffered() ||
                std::chrono::duration<double, std::milli>(now - lastSound).count() < holdMs;
            const float wanted = speaking ? std::clamp(duck_.load(), 0.0f, 1.0f) : 1.0f;
            const float step = float(stepMs / (wanted < duck ? duckDownMs : duckUpMs));
            const float next = wanted < duck ? std::max(wanted, duck - step) : std::min(wanted, duck + step);
            if (next != duck) {
                duck = next;
                sink::setVoiceDuck(duck);
            }
        }
        if (stream.client) stream.client->Stop();
    }
    sink::setVoiceDuck(1.0f);
    CloseHandle(event);
    if (com) CoUninitialize();
}

}
