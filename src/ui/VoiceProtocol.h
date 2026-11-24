// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// The pipe between Veyra and its voice worker ("Veyra.exe --voice-worker"), which turns
// translated lines into speech. Both ends are the same x64 Windows executable; integers and
// PCM are native little-endian.
//
// Veyra -> worker: Config (JSON: folder, runtime, cache, threads, and "listen" with the models
// that tell speakers apart) once, then Say (JSON: text, voice, speed) per line, Drop (forget
// queued lines with id <= the frame's id), Quit; when listening, Opening (the game's sound since
// a line appeared, 16 kHz mono float) and Ended (the whole sound of a line that left the
// screen), and Forget (a new session: the voices heard so far no longer count).
// Worker -> Veyra: Ready (JSON: voices, runtime, listening) once the models are loaded, then for
// every line some Audio frames (mono float PCM, 44.1 kHz) and Done, or Failed (UTF-8 message);
// Heard (JSON: female, voice, speech) for an Opening, Learnt (JSON: voice, female, isNew, gone,
// born) for an Ended.
#include <QByteArray>
#include <cstdint>

namespace veyra::ui::voice {

enum class Frame : uint32_t { Config = 1, Say, Drop, Quit, Opening, Ended, Forget, Stress,
                              Ready = 16, Audio, Done, Failed, Heard, Learnt };
inline constexpr uint32_t frameMagic = 0x53545456; // 'VTTS'
inline constexpr uint32_t maxPayload = 1024 * 1024; // 16 s of 16 kHz float PCM
inline constexpr int listenRate = 16000;
inline constexpr int sampleRate = 44100;

struct FrameHeader {
    uint32_t magic = frameMagic;
    Frame type = Frame::Say;
    uint64_t id = 0;
    uint32_t bytes = 0;
    uint32_t reserved = 0;
};
static_assert(sizeof(FrameHeader) == 24);

inline bool validHeader(const FrameHeader& header) {
    return header.magic == frameMagic && header.bytes <= maxPayload && header.reserved == 0;
}

inline QByteArray frame(Frame type, uint64_t id, QByteArrayView data = {}) {
    if (data.size() > maxPayload) return {};
    const FrameHeader header{frameMagic, type, id, uint32_t(data.size()), 0};
    QByteArray result(reinterpret_cast<const char*>(&header), sizeof(header));
    result.append(data.data(), data.size());
    return result;
}

int runWorker();

}
