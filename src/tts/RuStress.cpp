// SPDX-License-Identifier: GPL-3.0-only
#include "RuStress.h"
#include "Gzip.h"
#include "Ort.h"
#include "Tokenizers.h"
#include "Utf8.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <numeric>
#include <sstream>

namespace veyra::tts {
namespace {

constexpr char cacheMagic[8] = {'V', 'T', 'S', 'C', 'A', 'C', 'H', '1'};
constexpr float modelConfidence = 0.55f; // as RUAccent's character model is used in TeraTTSv2

std::string readFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + file.string());
    std::ostringstream data;
    data << in.rdbuf();
    return std::move(data).str();
}

std::string readGzip(const std::filesystem::path& file) {
    auto data = gunzip(readFile(file));
    if (!data) throw std::runtime_error("damaged " + file.string());
    return std::move(*data);
}

// Calls `pair(key, value)` for every "key": "value" of a flat JSON object of strings (the
// dictionaries have no escapes; a backslash escape is taken as the character after it).
template <class F>
void forEachPair(std::string_view json, F&& pair) {
    size_t i = json.find('{');
    if (i == std::string_view::npos) return;
    ++i;
    std::string strings[2];
    auto readString = [&](std::string& out) {
        out.clear();
        while (i < json.size() && json[i] != '"') ++i;
        if (i >= json.size()) return false;
        for (++i; i < json.size() && json[i] != '"'; ++i) {
            if (json[i] == '\\' && i + 1 < json.size()) ++i;
            out.push_back(json[i]);
        }
        ++i;
        return i <= json.size();
    };
    for (;;) {
        while (i < json.size() && json[i] != '"' && json[i] != '}') ++i;
        if (i >= json.size() || json[i] == '}') return;
        if (!readString(strings[0])) return;
        while (i < json.size() && json[i] != ':') ++i;
        if (!readString(strings[1])) return;
        pair(strings[0], strings[1]);
    }
}

// Calls `list(key, values)` for every "key": ["value", ...] of a flat JSON object.
template <class F>
void forEachList(std::string_view json, F&& list) {
    size_t i = json.find('{');
    if (i == std::string_view::npos) return;
    ++i;
    auto readString = [&](std::string& out) {
        out.clear();
        for (++i; i < json.size() && json[i] != '"'; ++i) {
            if (json[i] == '\\' && i + 1 < json.size()) ++i;
            out.push_back(json[i]);
        }
        ++i;
    };
    std::string key, value;
    std::vector<std::string> values;
    for (;;) {
        while (i < json.size() && json[i] != '"' && json[i] != '}') ++i;
        if (i >= json.size() || json[i] == '}') return;
        readString(key);
        while (i < json.size() && json[i] != '[') ++i;
        values.clear();
        for (++i; i < json.size() && json[i] != ']'; ++i) {
            if (json[i] != '"') continue;
            readString(value);
            values.push_back(value);
            --i;
        }
        ++i;
        list(key, values);
    }
}

// Letters of `target` in the case of the matching letters of `source` (same length only).
std::u32string fixCapital(std::u32string_view source, std::u32string_view target) {
    if (source.size() != target.size()) return std::u32string(target);
    std::u32string out(target);
    for (size_t i = 0; i < out.size(); ++i) out[i] = isUpper(source[i]) ? toUpper(out[i]) : toLower(out[i]);
    return out;
}

int vowels(std::u32string_view word) {
    return int(std::count_if(word.begin(), word.end(), isRussianVowel));
}

// `marked` (a lower-case reading with "+" marks) in the letter case of `source`.
std::u32string like(std::u32string_view source, std::u32string_view marked) {
    std::u32string out;
    size_t at = 0;
    for (const char32_t c : marked) {
        if (c == U'+') { out.push_back(c); continue; }
        const char32_t original = at < source.size() ? source[at++] : c;
        out.push_back(isUpper(original) ? toUpper(c) : c);
    }
    return out;
}

bool isWordChar(char32_t c) { return isLetter(c) || isDigit(c) || c == U'_'; }

// What RUAccent keeps of a text: Russian and Latin letters, digits, spaces and common marks.
bool wanted(char32_t c) {
    static constexpr std::u32string_view marks = U"—.,!?:;'(){}[]«»„“”-";
    return isLetter(c) || isDigit(c) || isSpace(c) || c == U'\f' || c == U'\v' || marks.find(c) != std::u32string_view::npos;
}
bool endsSentence(char32_t c) { return c == U'.' || c == U'!' || c == U'?' || c == U'…'; }

// Readings settled by hand, before any network is asked: where RUAccent's networks are wrong
// on lines of games ("ага" came out as the Turkish "+ага"; "чё" was left without its stress).
const std::unordered_map<std::u32string, std::u32string>& fixes() {
    static const std::unordered_map<std::u32string, std::u32string> table{
        {U"ага", U"аг+а"}, {U"че", U"ч+ё"}, {U"чё", U"ч+ё"}, {U"нифига", U"нифиг+а"}, {U"ниче", U"нич+ё"},
        {U"ничё", U"нич+ё"}, {U"щас", U"щ+ас"},
    };
    return table;
}
// The sky, not the palate; "the best", not the bookish "she herself": the "ё" network is sure
// of the other reading whatever the sentence.
bool keepsE(std::u32string_view lower) {
    return lower == U"небо" || lower == U"неба" || lower == U"небу" || lower == U"небом" || lower == U"самое";
}
// "Черт" is the devil, unless it is "features": "тонких черт", "нет черт".
bool beforeFeatures(std::u32string_view lower) {
    static constexpr std::u32string_view words[]{U"нет", U"без", U"из", U"много", U"мало", U"несколько", U"ряд",
        U"этих", U"тех", U"таких", U"всех", U"его", U"ее", U"её", U"их"};
    if (std::find(std::begin(words), std::end(words), lower) != std::end(words)) return true;
    return lower.size() >= 2 && (lower.ends_with(U"ых") || lower.ends_with(U"их"));
}

std::vector<float> softmax(const float* logits, size_t n) {
    std::vector<float> out(logits, logits + n);
    const float top = *std::max_element(out.begin(), out.end());
    float sum = 0;
    for (auto& v : out) sum += (v = std::exp(v - top));
    for (auto& v : out) v /= sum;
    return out;
}

} // namespace

// A network that labels each word of a sentence (punctuation marks count as words): the label
// of a word is the one most likely on average over its pieces.
struct RuStress::Tagger {
    Tagger(const OrtRuntime& runtime, const std::filesystem::path& folder, std::u32string_view wantedLabel)
        : text(folder / "tokenizer.json"), model(runtime, folder / "model.onnx", 2, false) {
        const auto config = readFile(folder / "config.json");
        const auto labels = config.find("\"id2label\"");
        if (labels == std::string::npos) throw std::runtime_error("no labels in " + (folder / "config.json").string());
        forEachPair(std::string_view(config).substr(labels + 10), [&](const std::string& id, const std::string& label) {
            if (decodeUtf8(label) == wantedLabel) wanted = std::stoi(id);
        });
        if (wanted < 0) throw std::runtime_error("no label in " + (folder / "config.json").string());
        for (const auto& name : model.inputNames()) if (name == "token_type_ids") types = true;
    }
    // For each word: whether its label is the one asked about.
    std::vector<bool> labels(std::u32string_view sentence) const {
        const auto encoded = text.encode(sentence);
        const auto n = int64_t(encoded.ids.size());
        const auto ids = Tensor::of({1, n}, encoded.ids);
        const auto mask = Tensor::of({1, n}, std::vector<int64_t>(size_t(n), 1));
        const auto zeros = Tensor::of({1, n}, std::vector<int64_t>(size_t(n), 0));
        std::vector<std::pair<const char*, const Tensor*>> inputs{{"input_ids", &ids}, {"attention_mask", &mask}};
        if (types) inputs.emplace_back("token_type_ids", &zeros);
        const auto logits = model.run(inputs, "logits");
        if (logits.shape.size() != 3 || logits.shape[1] != n) throw std::runtime_error("unexpected tagger output");
        const auto width = size_t(logits.shape[2]);
        std::vector<std::vector<float>> sums;
        std::vector<int> counts;
        for (size_t k = 0; k < size_t(n); ++k) {
            if (encoded.special[k]) continue;
            const auto row = softmax(logits.floats.data() + k * width, width);
            if (!sums.empty() && encoded.continues[k]) {
                for (size_t j = 0; j < width; ++j) sums.back()[j] += row[j];
                ++counts.back();
            } else {
                sums.push_back(row);
                counts.push_back(1);
            }
        }
        std::vector<bool> out;
        for (const auto& sum : sums)
            out.push_back(int(std::max_element(sum.begin(), sum.end()) - sum.begin()) == wanted);
        return out;
    }
    BertTokenizer text;
    OrtModel model;
    int wanted = -1;
    bool types = false;
};

RuStress::RuStress(const std::filesystem::path& folder, const std::filesystem::path& cache, const OrtRuntime* runtime) {
    const auto dictionary = folder / "dictionary";
    if (cache.empty() || !loadCache(cache)) {
        buildFromJson(dictionary / "accents.json.gz");
        if (!cache.empty()) saveCache(cache);
    }
    forEachPair(readGzip(dictionary / "yo_words.json.gz"), [&](const std::string& key, const std::string& value) {
        yo_.emplace(decodeUtf8(key), decodeUtf8(value));
    });
    if (!runtime) return;
    const auto nn = folder / "nn" / "nn_accent";
    std::istringstream vocab(readFile(nn / "vocab.txt"));
    std::string line;
    for (int64_t index = 0; std::getline(vocab, line); ++index) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "[unk]") unk_ = index;
        else if (line == "[bos]") bos_ = index;
        else if (line == "[eos]") eos_ = index;
        const auto chars = decodeUtf8(line);
        if (chars.size() == 1) vocab_[chars[0]] = index;
    }
    model_ = std::make_unique<OrtModel>(*runtime, nn / "model.onnx", 1);

    // The whole of RUAccent, when it was downloaded.
    const auto networks = folder / "nn";
    const auto homographFolder = networks / "nn_omograph" / "turbo3.1";
    std::error_code missing;
    if (!std::filesystem::exists(homographFolder / "model.onnx", missing)
        || !std::filesystem::exists(networks / "nn_stress_usage_predictor" / "model.onnx", missing)
        || !std::filesystem::exists(networks / "nn_yo_homograph_resolver" / "model.onnx", missing)
        || !std::filesystem::exists(dictionary / "omographs.json.gz", missing)
        || !std::filesystem::exists(dictionary / "yo_homographs.json.gz", missing))
        return;
    try {
        forEachList(readGzip(dictionary / "omographs.json.gz"), [&](const std::string& key, const std::vector<std::string>& values) {
            auto& list = readings_[decodeUtf8(key)];
            for (const auto& v : values) list.push_back(decodeUtf8(v));
        });
        readings_[U"коса"] = {U"к+оса", U"кос+а"}; // (as RUAccent adds it)
        forEachPair(readGzip(dictionary / "yo_homographs.json.gz"), [&](const std::string& key, const std::string& value) {
            yoReadings_.emplace(decodeUtf8(key), decodeUtf8(value));
        });
        homographText_ = std::make_unique<ByteBpeTokenizer>(homographFolder / "tokenizer.json");
        homographs_ = std::make_unique<OrtModel>(*runtime, homographFolder / "model.onnx", 2, false);
        yoTagger_ = std::make_unique<Tagger>(*runtime, networks / "nn_yo_homograph_resolver", U"YO");
        carries_ = std::make_unique<Tagger>(*runtime, networks / "nn_stress_usage_predictor", U"STRESS");
    } catch (const std::exception&) {
        // A damaged network must not silence the voice: the dictionaries alone, word by word.
        carries_.reset();
        yoTagger_.reset();
        homographs_.reset();
        homographText_.reset();
        readings_.clear();
        yoReadings_.clear();
    }
}

RuStress::~RuStress() = default;

void RuStress::buildFromJson(const std::filesystem::path& file) {
    std::vector<std::pair<uint64_t, uint8_t>> entries;
    entries.reserve(3300000);
    forEachPair(readGzip(file), [&](const std::string& key, const std::string& value) {
        const auto accented = decodeUtf8(value);
        const auto plus = accented.find(U'+');
        if (plus == std::u32string::npos || plus > 255) return;
        entries.emplace_back(wordHash(decodeUtf8(key)), uint8_t(plus));
    });
    // The "о" interjection, as RUAccent adds it to the dictionary.
    entries.emplace_back(wordHash(U"о"), uint8_t(0));
    std::sort(entries.begin(), entries.end());
    entries.erase(std::unique(entries.begin(), entries.end(),
        [](const auto& a, const auto& b) { return a.first == b.first; }), entries.end());
    keys_.resize(entries.size());
    stress_.resize(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        keys_[i] = entries[i].first;
        stress_[i] = entries[i].second;
    }
}

bool RuStress::loadCache(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    char magic[8]{};
    uint64_t count = 0;
    if (!in.read(magic, 8) || std::memcmp(magic, cacheMagic, 8) || !in.read(reinterpret_cast<char*>(&count), 8))
        return false;
    if (count == 0 || count > 20000000) return false;
    keys_.resize(count);
    stress_.resize(count);
    if (!in.read(reinterpret_cast<char*>(keys_.data()), std::streamsize(count * 8)) ||
        !in.read(reinterpret_cast<char*>(stress_.data()), std::streamsize(count)) ||
        !std::is_sorted(keys_.begin(), keys_.end())) {
        keys_.clear();
        stress_.clear();
        return false;
    }
    return true;
}

void RuStress::saveCache(const std::filesystem::path& file) const {
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    auto temporary = file;
    temporary += ".part";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        const uint64_t count = keys_.size();
        out.write(cacheMagic, 8);
        out.write(reinterpret_cast<const char*>(&count), 8);
        out.write(reinterpret_cast<const char*>(keys_.data()), std::streamsize(count * 8));
        out.write(reinterpret_cast<const char*>(stress_.data()), std::streamsize(count));
        if (!out) return;
    }
    std::filesystem::rename(temporary, file, error);
}

int RuStress::dictionaryStress(std::u32string_view lower) const {
    const auto hash = wordHash(lower);
    const auto found = std::lower_bound(keys_.begin(), keys_.end(), hash);
    if (found == keys_.end() || *found != hash) return -1;
    const int at = stress_[size_t(found - keys_.begin())];
    return at < int(lower.size()) ? at : -1;
}

int RuStress::modelStress(std::u32string_view lower) const {
    if (!model_ || lower.size() > 38) return -1; // the model was trained on words up to 40 tokens
    std::vector<int64_t> ids{bos_};
    for (const char32_t c : lower) {
        const auto found = vocab_.find(c);
        ids.push_back(found == vocab_.end() ? unk_ : found->second);
    }
    ids.push_back(eos_);
    const auto n = int64_t(ids.size());
    const auto input = Tensor::of({1, n}, ids);
    const auto mask = Tensor::of({1, n}, std::vector<int64_t>(size_t(n), 1));
    const auto types = Tensor::of({1, n}, std::vector<int64_t>(size_t(n), 0));
    const auto logits = model_->run({{"input_ids", &input}, {"attention_mask", &mask}, {"token_type_ids", &types}},
                                    "logits");
    if (logits.shape.size() != 3 || logits.shape[1] != n || logits.shape[2] != 3) return -1;
    // The letter whose primary-stress probability is highest, if it is confident enough.
    int best = -1;
    float bestScore = modelConfidence;
    for (int64_t position = 1; position + 1 < n; ++position) {
        const float* l = logits.floats.data() + position * 3;
        const float top = std::max({l[0], l[1], l[2]});
        if (l[1] < top) continue; // primary stress is not this letter's most likely label
        const float sum = std::exp(l[0] - top) + std::exp(l[1] - top) + std::exp(l[2] - top);
        const float score = 1.0f / sum;
        if (score >= bestScore && isRussianVowel(lower[size_t(position - 1)])) {
            bestScore = score;
            best = int(position - 1);
        }
    }
    return best;
}

std::u32string RuStress::markWord(std::u32string_view original) const {
    if (const auto* fixed = fixFor(toLower(original))) return like(original, *fixed);
    std::u32string word(original);
    if (const auto yo = yo_.find(toLower(word)); yo != yo_.end()) word = fixCapital(word, yo->second);
    const auto lower = toLower(word);
    // A word of one vowel has nowhere else to put its stress; marking it makes prepositions and
    // particles ("на", "в", "же") sound emphasised, which RUAccent avoids with a model of its own.
    if (vowels(lower) < 2) return word;
    int at = dictionaryStress(lower);
    if (at < 0) {
        try {
            at = modelStress(lower);
        } catch (const std::exception&) {
            at = -1; // one odd word must not silence the line
        }
    }
    if (at >= 0) word.insert(size_t(at), 1, U'+');
    return word;
}

float RuStress::knownShare(std::u32string_view text, int& counted) const {
    counted = 0;
    std::vector<std::u32string_view> list;
    for (size_t i = 0; i < text.size();) {
        while (i < text.size() && !isLetter(text[i])) ++i;
        const size_t start = i;
        while (i < text.size() && (isLetter(text[i]) || text[i] == U'-')) ++i;
        if (start == i) break;
        const auto word = text.substr(start, i - start);
        if (std::all_of(word.begin(), word.end(), [](char32_t c) { return isCyrillic(c) || c == U'-'; })
            && vowels(toLower(word)) >= 2)
            list.push_back(word);
    }
    // Every word with a capital is a title or a sign, not speech: those are judged too.
    const bool titled = list.size() >= 3 && std::all_of(list.begin(), list.end(), [](auto w) { return isUpper(w.front()); });
    int known = 0;
    for (size_t index = 0; index < list.size(); ++index) {
        const auto word = list[index];
        if (index > 0 && !titled && isUpper(word.front())) continue; // a name: the dictionary need not know it
        const auto lower = toLower(word);
        ++counted;
        if (const auto yo = yo_.find(lower); yo != yo_.end()) { ++known; continue; }
        if (dictionaryStress(lower) >= 0) ++known;
    }
    return counted ? float(known) / float(counted) : 1.0f;
}

bool RuStress::validFix(std::u32string_view reading) {
    const auto plus = reading.find(U'+');
    if (plus == std::u32string_view::npos || reading.find(U'+', plus + 1) != std::u32string_view::npos) return false;
    if (plus + 1 >= reading.size() || !isRussianVowel(reading[plus + 1])) return false;
    if (reading.size() > 40) return false;
    return std::all_of(reading.begin(), reading.end(), [](char32_t c) { return isCyrillic(c) || c == U'+' || c == U'-'; });
}

void RuStress::setUserFixes(const std::vector<std::u32string>& readings) {
    userFixes_.clear();
    for (const auto& reading : readings) {
        if (!validFix(reading)) continue;
        const auto lower = toLower(reading);
        std::u32string word;
        for (const char32_t c : lower) if (c != U'+') word.push_back(c);
        userFixes_[word] = lower;
        // "ё" written or not, the same word: "ч+ёрт" is found for "черт" as well.
        std::u32string plainE = word;
        std::replace(plainE.begin(), plainE.end(), U'ё', U'е');
        userFixes_.emplace(plainE, lower);
    }
}

const std::u32string* RuStress::fixFor(std::u32string_view lower) const {
    if (const auto user = userFixes_.find(std::u32string(lower)); user != userFixes_.end()) return &user->second;
    if (const auto own = fixes().find(std::u32string(lower)); own != fixes().end()) return &own->second;
    return nullptr;
}

std::u32string RuStress::chooseReading(std::u32string_view sentence, const std::vector<std::u32string>& readings) const {
    // No space before a mark: "слово ," -> "слово,".
    std::u32string tight;
    for (size_t i = 0; i < sentence.size(); ++i) {
        if (isSpace(sentence[i])) {
            size_t j = i;
            while (j < sentence.size() && isSpace(sentence[j])) ++j;
            if (j < sentence.size() && std::u32string_view(U",.?!:;…").find(sentence[j]) != std::u32string_view::npos) {
                i = j - 1;
                continue;
            }
        }
        tight.push_back(sentence[i]);
    }
    size_t best = 0;
    float bestFit = -1;
    for (size_t k = 0; k < readings.size(); ++k) {
        const auto ids = homographText_->encodePair(tight, readings[k]);
        const auto n = int64_t(ids.size());
        const auto input = Tensor::of({1, n}, ids);
        const auto mask = Tensor::of({1, n}, std::vector<int64_t>(size_t(n), 1));
        const auto logits = homographs_->run({{"input_ids", &input}, {"attention_mask", &mask}}, "logits");
        if (logits.floats.size() < 2) continue;
        const float fit = softmax(logits.floats.data(), 2)[1];
        if (fit > bestFit) { bestFit = fit; best = k; }
    }
    return readings[best];
}

// One sentence, as RUAccent marks it.
std::u32string RuStress::markSentence(std::u32string_view sentence) const {
    // A dash between words is not a word.
    std::u32string spaced;
    for (size_t i = 0; i < sentence.size(); ++i) {
        if (sentence.substr(i, 3) == U" - ") { spaced += U" ~ "; i += 2; }
        else spaced.push_back(sentence[i]);
    }
    // Words (\w*(?:\+\w+)*) and runs of marks ([^\w\s]+), and the gaps between them.
    std::vector<std::u32string> words, gaps;
    size_t i = 0, gapStart = 0;
    while (i < spaced.size()) {
        const size_t start = i;
        if (isWordChar(spaced[i]) || (spaced[i] == U'+' && i + 1 < spaced.size() && isWordChar(spaced[i + 1]))) {
            while (i < spaced.size() && isWordChar(spaced[i])) ++i;
            while (i + 1 < spaced.size() && spaced[i] == U'+' && isWordChar(spaced[i + 1])) {
                ++i;
                while (i < spaced.size() && isWordChar(spaced[i])) ++i;
            }
        } else if (!isSpace(spaced[i]) && spaced[i] != U'\f' && spaced[i] != U'\v') {
            while (i < spaced.size() && !isWordChar(spaced[i]) && !isSpace(spaced[i])) ++i;
        } else {
            ++i;
            continue;
        }
        gaps.push_back(spaced.substr(gapStart, start - gapStart));
        words.push_back(spaced.substr(start, i - start));
        gapStart = i;
    }
    if (words.empty()) return std::u32string(sentence);
    gaps.push_back(spaced.substr(gapStart));

    const auto carries = carries_->labels(sentence);
    const auto lowered = toLower(sentence);
    const auto yo = lowered.find(U'е') != std::u32string::npos ? yoTagger_->labels(lowered) : std::vector<bool>{};
    for (size_t index = 0; index < words.size(); ++index) {
        const auto& word = words[index];
        const auto lower = toLower(word);
        std::u32string fixed;
        if (const auto* f = fixFor(lower)) fixed = *f;
        else if (lower == U"черт" && (index == 0 || !beforeFeatures(toLower(words[index - 1])))) fixed = U"ч+ёрт";
        if (!fixed.empty()) { words[index] = like(word, fixed); continue; }
        std::u32string written = word;
        if (const auto y = yo_.find(lower); y != yo_.end()) written = fixCapital(word, y->second);
        if (index < yo.size() && yo[index] && !keepsE(lower))
            if (const auto y = yoReadings_.find(lower); y != yoReadings_.end()) written = fixCapital(word, y->second);
        words[index] = written;
    }
    for (size_t index = 0; index < words.size(); ++index) {
        const auto found = readings_.find(toLower(words[index]));
        if (found == readings_.end() || found->second.empty()) continue;
        std::u32string context;
        for (size_t k = 0; k < words.size(); ++k) {
            if (k) context.push_back(U' ');
            context += k == index ? U" <w>" + words[k] + U"</w> " : words[k];
        }
        try {
            words[index] = like(words[index], chooseReading(context, found->second));
        } catch (const std::exception&) {
            // one odd sentence must not silence the line: the word goes on without the network
        }
    }
    for (size_t index = 0; index < words.size(); ++index) {
        auto& word = words[index];
        if (word.find(U'+') != std::u32string::npos || index >= carries.size() || !carries[index]) continue;
        const auto lower = toLower(word);
        if (const int at = dictionaryStress(lower); at >= 0) {
            word.insert(size_t(at), 1, U'+');
        } else if (std::all_of(lower.begin(), lower.end(), [](char32_t c) { return isLetter(c) || isDigit(c) || c == U'_'; })
                   && vowels(lower) > 1) {
            try {
                if (const int guess = modelStress(lower); guess >= 0) word.insert(size_t(guess), 1, U'+');
            } catch (const std::exception&) {
            }
        }
    }
    std::u32string out;
    for (size_t k = 0; k < words.size(); ++k) out += gaps[k] + words[k];
    out += gaps.back();
    // The dash back.
    std::u32string result;
    for (size_t k = 0; k < out.size(); ++k) {
        if (out.substr(k, 3) == U" ~ ") { result += U" - "; k += 2; }
        else result.push_back(out[k] == U'~' ? U'-' : out[k]);
    }
    return result;
}

std::u32string RuStress::mark(std::u32string_view text) const {
    // Stress the caller placed itself is authoritative.
    if (text.find(U'+') != std::u32string_view::npos) return std::u32string(text);
    if (full()) {
        std::u32string kept;
        for (const char32_t c : text) if (wanted(c)) kept.push_back(c);
        // Sentence by sentence ([^.!?…]+[.!?…]*["»“]*); what lies between is kept as it is.
        std::u32string out;
        size_t i = 0;
        while (i < kept.size()) {
            if (endsSentence(kept[i])) { out.push_back(kept[i++]); continue; }
            const size_t start = i;
            while (i < kept.size() && !endsSentence(kept[i])) ++i;
            while (i < kept.size() && endsSentence(kept[i])) ++i;
            while (i < kept.size() && (kept[i] == U'"' || kept[i] == U'»' || kept[i] == U'“')) ++i;
            const auto sentence = std::u32string_view(kept).substr(start, i - start);
            try {
                out += markSentence(sentence);
            } catch (const std::exception&) {
                for (size_t k = start; k < i;) { // the networks failed: the word-by-word way
                    if (!isCyrillic(kept[k])) { out.push_back(kept[k++]); continue; }
                    size_t end = k;
                    while (end < i && isCyrillic(kept[end])) ++end;
                    out += markWord(std::u32string_view(kept).substr(k, end - k));
                    k = end;
                }
            }
        }
        return out;
    }
    std::u32string out;
    out.reserve(text.size() + text.size() / 4);
    for (size_t i = 0; i < text.size();) {
        if (!isCyrillic(text[i])) {
            out.push_back(text[i++]);
            continue;
        }
        size_t end = i;
        while (end < text.size() && isCyrillic(text[end])) ++end;
        out += markWord(text.substr(i, end - i));
        i = end;
    }
    return out;
}

} // namespace veyra::tts
