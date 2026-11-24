// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// The two kinds of Hugging Face tokenizer that RUAccent's context networks were trained with,
// read from their tokenizer.json and producing the same token ids as the `tokenizers` library:
//   * BERT WordPiece (BertNormalizer without lower-casing, BertPreTokenizer, [CLS] A [SEP]) -
//     the networks that say which words carry a stress and which "е" is "ё";
//   * RoBERTa byte-level BPE with added tokens (<s> A </s></s> B </s>) - the network that picks
//     the reading of a homograph.
// Only what those files use is implemented. Portable C++, no Qt.
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace veyra::tts {

struct BertEncoding {
    std::vector<int64_t> ids;
    std::vector<bool> special;   // [CLS] / [SEP]
    std::vector<bool> continues; // a "##" piece that continues the word before it
};

class BertTokenizer {
public:
    explicit BertTokenizer(const std::filesystem::path& file);
    BertEncoding encode(std::u32string_view text) const;
    int64_t unknown() const { return unk_; }

private:
    void piece(std::u32string_view word, BertEncoding& out) const;
    std::unordered_map<std::u32string, int64_t> vocab_;
    int64_t unk_ = 1, cls_ = 2, sep_ = 3;
    size_t maxChars_ = 100;
};

class ByteBpeTokenizer {
public:
    explicit ByteBpeTokenizer(const std::filesystem::path& file);
    // <s> a </s></s> b </s>
    std::vector<int64_t> encodePair(std::u32string_view a, std::u32string_view b) const;
    std::vector<int64_t> encode(std::u32string_view text) const; // without the special tokens

private:
    void sequence(std::u32string_view text, std::vector<int64_t>& out) const;
    void word(std::string_view bytes, std::vector<int64_t>& out) const;
    std::unordered_map<std::u32string, int64_t> vocab_;
    std::unordered_map<std::u32string, int64_t> added_; // matched in the raw text first
    size_t longestAdded_ = 0;
    std::unordered_map<std::u32string, int> ranks_;     // "a b" -> merge rank
    char32_t byteChar_[256]{};
    int64_t bos_ = 0, eos_ = 2;
};

} // namespace veyra::tts
