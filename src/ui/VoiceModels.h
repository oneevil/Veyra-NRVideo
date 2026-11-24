// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// What the voice of live translation downloads: TeraTTSv2 (models, narrators, the stress
// dictionaries) and the ONNX Runtime it runs on, which is the runtime live speech
// recognition downloads too - so both live under the recognition's folder and share it.
#include "TranslationAssets.h"

namespace veyra::ui::voice {

QString root(const QString& dataDirectory);           // the folder ModelAssets downloads into
std::span<const ModelAsset> assets();
QString modelFolder(const QString& dataDirectory);    // TeraTTSv2's files
QString runtimeLibrary(const QString& dataDirectory); // onnxruntime.dll
QString cacheFolder(const QString& dataDirectory);    // the stress dictionary in quick-to-load form

// Telling speakers apart by the game's sound (optional, downloaded on its own): Silero VAD
// (shared with speech recognition), the voice gender classifier, the WeSpeaker fingerprint model.
std::span<const ModelAsset> listeningAssets();
QString vadModel(const QString& dataDirectory);
QString genderFolder(const QString& dataDirectory);
QString speakerModel(const QString& dataDirectory);

}
