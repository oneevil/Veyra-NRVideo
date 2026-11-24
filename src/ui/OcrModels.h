// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// What the neural subtitle reader downloads: PP-OCRv5 (PaddleOCR, Apache-2.0) in the ONNX form
// RapidOCR 3.10.0 publishes - the text finder and one reading model per script - and the ONNX
// Runtime it runs on, shared with speech recognition and the voice.
#include "TranslationAssets.h"

namespace veyra::ui::ocr {

std::span<const ModelAsset> assets();
QString modelFolder(const QString& dataDirectory);    // det/ and rec/ below it
QString runtimeLibrary(const QString& dataDirectory); // onnxruntime.dll
// The reading model for a language (BCP-47): "rec/eslav_..." for Russian, Ukrainian and
// Belarusian, "rec/en_..." for English, "rec/latin_..." for the other Latin alphabets; empty
// when PP-OCRv5's mobile models do not read that script (the Windows reader is used then).
QString recognitionModel(const QString& language);
QString detectionModel();

}
