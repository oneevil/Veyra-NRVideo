// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Stress marks for Russian text ("+" before the stressed vowel), which TeraTTS reads to put
// the stress where it belongs. A C++ port of RUAccent (MIT, Denis Petrov) in the form TeraTTSv2
// publishes it, run in full:
//   * the dictionaries: stress of known words, "е" that is always "ё" (еще -> ещё);
//   * a network that tells which words of a sentence carry a stress at all;
//   * a network that tells, from the sentence, an "е" that is "ё" here (все / всё, берет / берёт);
//   * a network that picks the reading of a homograph from the sentence (з+амок / зам+ок);
//   * the character model nn_accent for words no dictionary has (names, slang);
//   * a few readings settled by hand where the networks are known to be wrong ("ага", "чёрт").
// Without the three context networks (an older download) only the dictionaries and the
// character model are used, word by word.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace veyra::tts {

class OrtRuntime;
class OrtModel;
class BertTokenizer;
class ByteBpeTokenizer;

class RuStress {
public:
    // `folder`: TeraTTSv2's "ruaccent" directory. `cache`: where the dictionary is kept in
    // a quick-to-load form after the first start (empty: build it every time).
    // `runtime` may be null: then unknown words stay without a mark.
    RuStress(const std::filesystem::path& folder, const std::filesystem::path& cache, const OrtRuntime* runtime);
    ~RuStress();

    std::u32string mark(std::u32string_view text) const;
    size_t words() const { return keys_.size(); }
    // How much of a line the dictionary knows: the share of its Russian words of two vowels or
    // more (names with a capital inside the line left out) that are real words; `counted` how
    // many such words there were. A line read off the picture that is mostly unknown words is
    // the OCR misreading something that is not a subtitle (credits, signs, the HUD).
    // A line of words that all begin with a capital is judged whole (titles, not speech).
    float knownShare(std::u32string_view text, int& counted) const;

    // Whether the context networks are in use (the whole of RUAccent).
    bool full() const { return carries_ != nullptr; }
    // The user's own readings, each a word with "+" before its stressed vowel ("Тр+евор"): they
    // win over everything else. Readings without exactly one "+" before a vowel are ignored.
    void setUserFixes(const std::vector<std::u32string>& readings);
    // Whether a reading is one setUserFixes takes.
    static bool validFix(std::u32string_view reading);

private:
    struct Tagger;
    std::u32string markWord(std::u32string_view word) const;
    std::u32string markSentence(std::u32string_view sentence) const;
    std::u32string chooseReading(std::u32string_view sentence, const std::vector<std::u32string>& readings) const;
    int dictionaryStress(std::u32string_view lower) const; // -1 if unknown
    int modelStress(std::u32string_view lower) const;      // -1 if unsure
    bool loadCache(const std::filesystem::path& file);
    void saveCache(const std::filesystem::path& file) const;
    void buildFromJson(const std::filesystem::path& file);

    std::vector<uint64_t> keys_;   // sorted word hashes
    std::vector<uint8_t> stress_;  // index of the stressed letter, by key
    std::unordered_map<std::u32string, std::u32string> yo_;
    std::unique_ptr<OrtModel> model_;
    std::unordered_map<char32_t, int64_t> vocab_;
    int64_t unk_ = 1, bos_ = 2, eos_ = 3;

    std::unique_ptr<Tagger> carries_, yoTagger_;
    std::unique_ptr<OrtModel> homographs_;
    std::unique_ptr<ByteBpeTokenizer> homographText_;
    std::unordered_map<std::u32string, std::vector<std::u32string>> readings_; // homograph -> its readings
    std::unordered_map<std::u32string, std::u32string> yoReadings_;            // все -> всё, when the sentence says so
    std::unordered_map<std::u32string, std::u32string> userFixes_;             // word -> the user's reading
    const std::u32string* fixFor(std::u32string_view lower) const;
};

} // namespace veyra::tts
