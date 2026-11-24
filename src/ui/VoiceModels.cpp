// SPDX-License-Identifier: GPL-3.0-only
#include "VoiceModels.h"
#include "AsrModels.h"

#include <QDir>

namespace veyra::ui::voice {
namespace {
// TeraTTSv2 at a fixed commit of https://huggingface.co/TeraSpace/TeraTTSv2 (TeraSpace,
// Open RAIL-M with an attribution condition; the stress files are RUAccent's, MIT).
// Only what the engine uses: the distilled sampler, eight narrators, and RUAccent whole (its
// dictionaries, its character model and the three networks that read the sentence).
constexpr ModelFile teraFiles[]{
    {"LICENSE", "9de0fb6f7d93e5329ac2195ad5c16166b9d66d5247ed5da9b5d9e81810f998e2", 4171},
    {"LICENSE-OPENRAIL", "0d944a9110fed9a9602d60e0423a272903e7bd21ab060490774efc77c2275e9f", 15007},
    {"RUACCENT_NOTICE.txt", "079aded52d6e28786b692c28ce8373412e8688490333d87588527b5b1ffe0da7", 1176},
    {"unicode_indexer.json", "c1ed0c0804b836bd0e38170ae7b5b00647d818c5f659fcf21fa0feea950aa04d", 262171},
    {"models/text_encoder.onnx", "d59b7911fe9e465988f26bedbf7c34ef9b2c5588d9b0da5f86d41340a3324a12", 27910305},
    {"models/duration_predictor.onnx", "15248b4751493fe48543f01164cd9b74d103b4483c63367a01c344442ebf03d8", 1572411},
    {"models/sampler_distilled_cfg3_8step.onnx", "921a050f19794bf97234ca8ffc0298559eaca466d9f7f3b72ec41dfefb37acc6", 256417643},
    {"models/vocoder.onnx", "d4e4cf9a9b9de8cca28f309c4cb7735327322b5a764ba61dfc3706c3c7942c07", 101416199},
    {"ruaccent/dictionary/accents.json.gz", "aa460ebba90de00fbbf3d41d121961f605b98667e45efb7920f127473b15515e", 20954156},
    {"ruaccent/dictionary/yo_words.json.gz", "a19fa89a964a0691d9fe4ee384783e3934904891843d8f59a1c480d67947a82a", 548914},
    {"ruaccent/nn/nn_accent/model.onnx", "4e393144e45626f6f1062a0784ef06f921b97321a8e7b87ac2a09a892286500a", 803402},
    {"ruaccent/nn/nn_accent/vocab.txt", "242882b2f27800e49a6babf46867dabf6dc9b6535b600776a68815fe4d4f4382", 140},
    {"ruaccent/nn/nn_accent/config.json", "e377f159a8fc8673e211f100d4a237d5fe3dceb3b6789a5a738bf50bda23be49", 841},
    // The networks that read the whole sentence: which words carry a stress, which "е" is "ё"
    // (все / всё), which reading of a homograph fits (з+амок / зам+ок), and their dictionaries.
    {"ruaccent/dictionary/omographs.json.gz", "04a9e81c68d65f65ba493fe0110f99e79087548c2beeec3032e2b66e28706f36", 219047},
    {"ruaccent/dictionary/yo_homographs.json.gz", "c4ee777bbbab87f9eac838f370ad92974e079d02b21903e480c54b5f0c8c60d1", 5747},
    {"ruaccent/nn/nn_omograph/turbo3.1/model.onnx", "2cb6a174c4cdb45bd3132b4f7c8a3779fc4b6869863180ed7d0e421bcd453dbd", 359306923},
    {"ruaccent/nn/nn_omograph/turbo3.1/tokenizer.json", "eb84a77af38a91f6f327486c7827d8541f8d446250f8d408909999b6be817afa", 5532343},
    {"ruaccent/nn/nn_stress_usage_predictor/config.json", "aa0638edf56254f123e593e96f034c2b5e68303ac207b29f64f0878dc4c22631", 822},
    {"ruaccent/nn/nn_stress_usage_predictor/model.onnx", "3d547500637b4ddfec8880ed6d1405fd50ee9d3f0131ef8a2a69dcf961dbefeb", 116473561},
    {"ruaccent/nn/nn_stress_usage_predictor/tokenizer.json", "ce51cde5df60ecb167abec4c90db11360482150628f7a91f3e316df6d6ff6f6f", 2413536},
    {"ruaccent/nn/nn_yo_homograph_resolver/config.json", "adf7fd2c67be1498071f447ec4a23dbe84904c49f95d1d3e6c7d9df269b9a2e9", 625},
    {"ruaccent/nn/nn_yo_homograph_resolver/model.onnx", "42cc85bf0c4b319dfe3d89fa17b162a92fd5e1c651a657cb3d5f44978d4e70ac", 14332169},
    {"ruaccent/nn/nn_yo_homograph_resolver/tokenizer.json", "f2bf174f2fe62ff11c28d56555f9c0cf16b24e162ef53bcf42ff85ebcf6ebfa8", 126952},
    {"styles/ru_m5/style_ttl.npy", "6eccc64350dcc23a8e790eebd7abbec24b0055e6451d975deffa899885e26e99", 51328},
    {"styles/ru_m5/style_dp.npy", "79741747bb75e0dac2357c089bc923926c4a34fe46b5ce9f85376b14b8e59bf9", 640},
    {"styles/ru_m1/style_ttl.npy", "0a49356abab63262e3183518eacadd9e073681f87b8bfd86f7b5aed0317fbd0c", 51328},
    {"styles/ru_m1/style_dp.npy", "0bbdbe5044f5682152b1fe399f7681424870e03023e857b9706863a6d73bf9e9", 640},
    {"styles/ru_f1/style_ttl.npy", "489cb6d10f3e2528d7ac4b3e4d8b01ca54d44b98d193dd26684b16c3a24bd9a7", 51328},
    {"styles/ru_f1/style_dp.npy", "189e1eabfccf8f0f76a6b10d84ba0fdde18de2ecc2c2e16e3a790f961db95382", 640},
    {"styles/ru_f2/style_ttl.npy", "4947fa632443f8ad8343399ac258f67f1e85e9594cf5d42ef7e18d6f6fa3e763", 51328},
    {"styles/ru_f2/style_dp.npy", "48c10c8c3e6f9016e4ff625425450b99772a9246ffed6b062d236f23863b27d1", 640},
    {"styles/eng_m4/style_ttl.npy", "a8d5016c1aade83d2bdec1e3aa86d0e421ff9e2fb7a781637e64f8a0d8ef663e", 51328},
    {"styles/eng_m4/style_dp.npy", "526724e17a6d0d917c18db0bac6ac97af15ae63312375129ef294462fbbfc0a2", 640},
    {"styles/eng_f5/style_ttl.npy", "b4cc8b6d271d6ee587b13af7bef8f0151821b24d6af546a1167fa885bd962496", 51328},
    {"styles/eng_f5/style_dp.npy", "9629dce9db38b4ab5163429c1eeb9a5771786e58e98e343ea6251bf93a9dfd60", 640},
    {"styles/eng_m3/style_ttl.npy", "f0ffd38329830d1556e1096d396c2a807626b9308eb12af84ea4733c712d8d01", 51328},
    {"styles/eng_m3/style_dp.npy", "dbcc78e880d2e7c6a45f4bff464ee90c9ba17a2599c9d3cc3dea2c0abcb94524", 640},
    {"styles/eng_f3/style_ttl.npy", "95fdfe490f5b3d3cf699495309177c405a38a89707bc9c711f93dc083fe099b6", 51328},
    {"styles/eng_f3/style_dp.npy", "8471b25d2db5281feebc6e03af5bfb413c647368d75cdd8cebc44be36def7948", 640},
};
constexpr ModelSource teraSources[]{
    {"https://hf-mirror.com/TeraSpace/TeraTTSv2/resolve/4c3bf2eb99c148cb657af10bd3bf717befb714c8/", true},
};
constexpr qint64 teraBytes = 908734005;
// Not constexpr: the runtime entry is copied from the recognition's (constant-initialised) table.
const ModelAsset bundle[]{
    asr::runtimeAsset(),
    {"voices/teratts-v2-4c3bf2e",
     "https://huggingface.co/TeraSpace/TeraTTSv2/resolve/4c3bf2eb99c148cb657af10bd3bf717befb714c8/",
     teraBytes, "", teraFiles, "", 0, false, teraSources, true},
};

// Jaesung Huh's voice-gender-classifier (MIT) at a fixed commit: the author's own weights file.
// Its network's wiring is built into Veyra (src/tts/GenderGraph.inc) and reads these weights.
constexpr ModelFile genderFiles[]{
    {"model.safetensors", "2d8e0be1fdf159d60d5087416e6f6277c5e30ce9e33a61c767a9a409e6c503c5", 61907512},
    {"README.md", "606bc63dc3c489c82fe339e2b29027cd91c92bfbe7529fc6e99419c52048e631", 2357},
};
constexpr ModelSource genderSources[]{
    {"https://hf-mirror.com/JaesungHuh/voice-gender-classifier/resolve/db1222153bd60337e900be22add7af180452adc0/", true},
};
constexpr ModelSource speakerSources[]{
    {"https://hf-mirror.com/Wespeaker/wespeaker-voxceleb-resnet221-LM/resolve/26732f43821f238bfb258c5ef54924a798e62118/voxceleb_resnet221_LM.onnx",
     false, 95037357, "e6b8c14c022288ca186de39f038a9bd9404be4f174ac8a6a3b8dbf229c8f607d"},
};
const ModelAsset listening[]{
    asr::vadAsset(),
    {"voices/voice-gender-classifier-db12221",
     "https://huggingface.co/JaesungHuh/voice-gender-classifier/resolve/db1222153bd60337e900be22add7af180452adc0/",
     61907512 + 2357, "", genderFiles, "", 0, false, genderSources, true},
    // WeSpeaker ResNet221 trained on VoxCeleb (Apache-2.0).
    {"voices/wespeaker-voxceleb-resnet221-LM.onnx",
     "https://huggingface.co/Wespeaker/wespeaker-voxceleb-resnet221-LM/resolve/26732f43821f238bfb258c5ef54924a798e62118/voxceleb_resnet221_LM.onnx",
     95037357, "e6b8c14c022288ca186de39f038a9bd9404be4f174ac8a6a3b8dbf229c8f607d", {}, "", 0, false, speakerSources},
};
}

QString root(const QString& dataDirectory) { return asr::root(dataDirectory); }
std::span<const ModelAsset> listeningAssets() { return listening; }
QString vadModel(const QString& dataDirectory) { return ModelAssets::path(root(dataDirectory), listening[0]); }
QString genderFolder(const QString& dataDirectory) { return ModelAssets::path(root(dataDirectory), listening[1]); }
QString speakerModel(const QString& dataDirectory) { return ModelAssets::path(root(dataDirectory), listening[2]); }
std::span<const ModelAsset> assets() { return bundle; }
QString modelFolder(const QString& dataDirectory) {
    return ModelAssets::path(root(dataDirectory), bundle[1]);
}
QString runtimeLibrary(const QString& dataDirectory) {
    return QDir(ModelAssets::path(root(dataDirectory), bundle[0])).filePath(QStringLiteral("onnxruntime.dll"));
}
QString cacheFolder(const QString& dataDirectory) {
    return QDir(root(dataDirectory)).filePath(QStringLiteral("voices/cache"));
}

}
