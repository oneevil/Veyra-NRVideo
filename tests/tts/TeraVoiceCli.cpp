// SPDX-License-Identifier: GPL-3.0-only
// Speaks one line with the voice engine and writes a WAV file: a check of src/tts on any
// platform, independent of Qt and of the rest of Veyra.
//
//   veyra_tts_cli <TeraTTSv2 folder> <onnxruntime library> <voice> <text> <out.wav> [noise.npy]
//
// Prints the text exactly as the networks get it and how long synthesis took.
#include "../../src/tts/Ort.h"
#include "../../src/tts/TeraVoice.h"
#include "../../src/tts/Utf8.h"
#include "CliArgs.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>

namespace {
void writeWav(const std::filesystem::path& path, const std::vector<float>& samples, int rate) {
    std::ofstream out(path, std::ios::binary);
    auto put32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto put16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = uint32_t(samples.size() * 2);
    out.write("RIFF", 4); put32(36 + bytes); out.write("WAVEfmt ", 8);
    put32(16); put16(1); put16(1); put32(uint32_t(rate)); put32(uint32_t(rate * 2)); put16(2); put16(16);
    out.write("data", 4); put32(bytes);
    for (const float s : samples) {
        const float clamped = s < -1.0f ? -1.0f : s > 1.0f ? 1.0f : s;
        put16(uint16_t(int16_t(clamped * 32767.0f)));
    }
}
}

int main(int argc, char** argv) {
    const CliArgs args(argc, argv);
    if (args.size() < 6) {
        std::fprintf(stderr, "usage: veyra_tts_cli <folder> <onnxruntime> <voice> <text> <out.wav> [noise.npy]\n");
        return 2;
    }
    try {
        using clock = std::chrono::steady_clock;
        const auto started = clock::now();
        veyra::tts::OrtRuntime runtime(args.paths[2]);
        veyra::tts::TeraVoice::Options options;
        if (args.size() > 6) options.testNoise = args.paths[6];
        veyra::tts::TeraVoice voice(args.paths[1], args.paths[5].parent_path(), runtime, options);
        const auto loaded = clock::now();
        const auto& text = args.utf8[4];
        std::printf("onnxruntime %s, voices:", runtime.version().c_str());
        for (const auto& name : voice.voices()) std::printf(" %s", name.c_str());
        std::printf("\nprepared: %s\n", veyra::tts::encodeUtf8(voice.prepare(veyra::tts::decodeUtf8(text))).c_str());
        const auto before = clock::now();
        const auto sound = voice.synthesize(text, args.utf8[3]);
        const auto after = clock::now();
        writeWav(args.paths[5], sound, veyra::tts::TeraVoice::sampleRate);
        auto ms = [](auto a, auto b) { return long(std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count()); };
        std::printf("load %ld ms, synthesis %ld ms, audio %.2f s\n", ms(started, loaded), ms(before, after),
                    double(sound.size()) / veyra::tts::TeraVoice::sampleRate);
        return sound.empty() ? 1 : 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
}
