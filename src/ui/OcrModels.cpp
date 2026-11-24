// SPDX-License-Identifier: GPL-3.0-only
#include "OcrModels.h"
#include "AsrModels.h"
#include "VoiceModels.h"

#include <QDir>

namespace veyra::ui::ocr {
namespace {
constexpr ModelFile files[]{
    {"det/ch_PP-OCRv5_det_mobile.onnx", "4d97c44a20d30a81aad087d6a396b08f786c4635742afc391f6621f5c6ae78ae", 4819576},
    {"rec/eslav_PP-OCRv5_rec_mobile.onnx", "08705d6721849b1347d26187f15a5e362c431963a2a62bfff4feac578c489aab", 7911802},
    {"rec/en_PP-OCRv5_rec_mobile.onnx", "c3461add59bb4323ecba96a492ab75e06dda42467c9e3d0c18db5d1d21924be8", 7872351},
    {"rec/latin_PP-OCRv5_rec_mobile.onnx", "b20bd37c168a570f583afbc8cd7925603890efbcdc000a59e22c269d160b5f5a", 7904513},
};
constexpr qint64 bytes = 4819576 + 7911802 + 7872351 + 7904513;
// Not constexpr: the runtime entry is copied from the recognition's (constant-initialised) table.
const ModelAsset bundle[]{
    asr::runtimeAsset(),
    {"ocr/pp-ocrv5-rapidocr-3.10.0", "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv5/",
     bytes, "", files, "", 0, false, {}, true},
};
}

std::span<const ModelAsset> assets() { return bundle; }
QString modelFolder(const QString& dataDirectory) { return ModelAssets::path(voice::root(dataDirectory), bundle[1]); }
QString runtimeLibrary(const QString& dataDirectory) { return voice::runtimeLibrary(dataDirectory); }
QString detectionModel() { return QStringLiteral("det/ch_PP-OCRv5_det_mobile.onnx"); }

QString recognitionModel(const QString& language) {
    const auto primary = language.section(QLatin1Char('-'), 0, 0).toLower();
    if (primary == QLatin1String("ru") || primary == QLatin1String("uk") || primary == QLatin1String("be"))
        return QStringLiteral("rec/eslav_PP-OCRv5_rec_mobile.onnx");
    if (primary.isEmpty() || primary == QLatin1String("en")) return QStringLiteral("rec/en_PP-OCRv5_rec_mobile.onnx");
    static const QStringList latin{"fr", "de", "it", "es", "pt", "pl", "nl", "sv", "da", "no", "nb", "fi", "cs", "sk",
                                   "sl", "hr", "hu", "ro", "tr", "id", "ms", "vi", "ca", "et", "lv", "lt"};
    if (latin.contains(primary)) return QStringLiteral("rec/latin_PP-OCRv5_rec_mobile.onnx");
    return {};
}

}
