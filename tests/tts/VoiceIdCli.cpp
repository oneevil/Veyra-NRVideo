// SPDX-License-Identifier: GPL-3.0-only
// Tells the voices of clips apart with src/tts/VoiceId, as the voice worker does for the
// lines of a game: for each clip (16 kHz mono float32 raw files) its opening is matched and
// then the whole line is learnt.
//
//   veyra_voiceid_cli <onnxruntime> <silero_vad.onnx> <gender folder> <speaker.onnx> clip.f32...
#include "../../src/tts/Ort.h"
#include "../../src/tts/VoiceId.h"
#include "CliArgs.h"

#include <chrono>
#include <cstdio>
#include <fstream>

int main(int argc, char** argv) {
    const CliArgs args(argc, argv);
    if (args.size() < 6) {
        std::fprintf(stderr, "usage: veyra_voiceid_cli <onnxruntime> <silero> <gender folder> <speaker.onnx> clip.f32...\n");
        return 2;
    }
    try {
        veyra::tts::OrtRuntime runtime(args.paths[1]);
        veyra::tts::VoiceListener listener(runtime, args.paths[2], args.paths[3], args.paths[4], 2, true);
        for (size_t i = 5; i < args.size(); ++i) {
            std::ifstream in(args.paths[i], std::ios::binary);
            std::vector<float> wave;
            float v;
            while (in.read(reinterpret_cast<char*>(&v), 4)) wave.push_back(v);
            const auto started = std::chrono::steady_clock::now();
            const std::vector<float> opening(wave.begin(), wave.begin() + std::ptrdiff_t(std::min<size_t>(wave.size(), 16000)));
            const auto heard = listener.opening(opening);
            const auto openingMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            std::optional<bool> female;
            float speechSeconds = 0.0f;
            const auto credited = listener.lineEnded(wave, female, &speechSeconds);
            const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            std::printf("%s: opening speech %.2fs %s (score %.2f, manner %+.2f) voice[%s] (%ld ms) | line speech %.2fs %s voice[%s]%s (%ld ms)\n",
                        args.paths[i].filename().string().c_str(),
                        heard.speechSeconds, heard.female ? (*heard.female ? "female" : "male") : "?",
                        heard.score.value_or(0.0f), heard.arousal.value_or(0.0f), heard.voice.c_str(),
                        long(openingMs), speechSeconds, female ? (*female ? "female" : "male") : "?", credited.voice.c_str(),
                        credited.isNew ? " new" : "", long(totalMs));
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
}
