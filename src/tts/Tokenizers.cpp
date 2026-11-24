// SPDX-License-Identifier: GPL-3.0-only
#include "Tokenizers.h"
#include "Utf8.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace veyra::tts {
namespace {

// ---- just enough JSON for tokenizer.json ------------------------------------------------

struct Json {
    enum class Kind { Null, Bool, Number, String, Array, Object } kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    std::string text; // UTF-8
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;

    const Json* operator[](std::string_view key) const {
        for (const auto& [name, value] : fields) if (name == key) return &value;
        return nullptr;
    }
};

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}
    Json value() {
        space();
        if (i_ >= s_.size()) fail();
        const char c = s_[i_];
        Json out;
        if (c == '{') {
            out.kind = Json::Kind::Object;
            ++i_;
            space();
            if (peek('}')) { ++i_; return out; }
            for (;;) {
                space();
                auto key = string();
                space();
                expect(':');
                out.fields.emplace_back(std::move(key), value());
                space();
                if (peek(',')) { ++i_; continue; }
                expect('}');
                return out;
            }
        }
        if (c == '[') {
            out.kind = Json::Kind::Array;
            ++i_;
            space();
            if (peek(']')) { ++i_; return out; }
            for (;;) {
                out.items.push_back(value());
                space();
                if (peek(',')) { ++i_; continue; }
                expect(']');
                return out;
            }
        }
        if (c == '"') { out.kind = Json::Kind::String; out.text = string(); return out; }
        if (s_.substr(i_, 4) == "true") { i_ += 4; out.kind = Json::Kind::Bool; out.boolean = true; return out; }
        if (s_.substr(i_, 5) == "false") { i_ += 5; out.kind = Json::Kind::Bool; return out; }
        if (s_.substr(i_, 4) == "null") { i_ += 4; return out; }
        const size_t start = i_;
        while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '-' || s_[i_] == '+'
                                  || s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E')) ++i_;
        if (start == i_) fail();
        out.kind = Json::Kind::Number;
        out.number = std::stod(std::string(s_.substr(start, i_ - start)));
        return out;
    }

private:
    [[noreturn]] void fail() const { throw std::runtime_error("tokenizer.json: bad JSON at " + std::to_string(i_)); }
    void space() { while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) ++i_; }
    bool peek(char c) const { return i_ < s_.size() && s_[i_] == c; }
    void expect(char c) { if (!peek(c)) fail(); ++i_; }
    unsigned hex4() {
        if (i_ + 4 > s_.size()) fail();
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            const char h = s_[i_++];
            v <<= 4;
            if (h >= '0' && h <= '9') v |= unsigned(h - '0');
            else if (h >= 'a' && h <= 'f') v |= unsigned(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') v |= unsigned(h - 'A' + 10);
            else fail();
        }
        return v;
    }
    std::string string() {
        expect('"');
        std::string out;
        while (i_ < s_.size() && s_[i_] != '"') {
            if (s_[i_] != '\\') { out.push_back(s_[i_++]); continue; }
            ++i_;
            if (i_ >= s_.size()) fail();
            const char e = s_[i_++];
            switch (e) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'u': {
                char32_t c = hex4();
                if (c >= 0xD800 && c < 0xDC00 && s_.substr(i_, 2) == "\\u") {
                    i_ += 2;
                    const unsigned low = hex4();
                    c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                }
                appendUtf8(out, c);
                break;
            }
            default: out.push_back(e); break; // \" \\ \/
            }
        }
        expect('"');
        return out;
    }
    std::string_view s_;
    size_t i_ = 0;
};

Json readJson(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + file.string());
    std::ostringstream data;
    data << in.rdbuf();
    const auto text = std::move(data).str();
    return Parser(text).value();
}

const Json& need(const Json* value, const char* what) {
    if (!value) throw std::runtime_error(std::string("tokenizer.json: no ") + what);
    return *value;
}

void readVocab(const Json& model, std::unordered_map<std::u32string, int64_t>& vocab) {
    for (const auto& [token, id] : need(model["vocab"], "vocab").fields) vocab[decodeUtf8(token)] = int64_t(id.number);
}

int64_t idOf(const std::unordered_map<std::u32string, int64_t>& vocab, std::u32string_view token) {
    const auto found = vocab.find(std::u32string(token));
    if (found == vocab.end()) throw std::runtime_error("tokenizer.json: no " + encodeUtf8(token));
    return found->second;
}

// What the BERT tokenizer takes for punctuation: ASCII marks and the Unicode punctuation the
// subtitles can hold (the text is filtered to Russian, Latin, digits and common marks first).
bool isBertPunctuation(char32_t c) {
    if ((c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126)) return true;
    return c == 0xA1 || c == 0xA7 || c == 0xAB || c == 0xB6 || c == 0xB7 || c == 0xBB || c == 0xBF
        || (c >= 0x2010 && c <= 0x2027) || (c >= 0x2030 && c <= 0x205E) || (c >= 0x3001 && c <= 0x3003)
        || (c >= 0x3008 && c <= 0x3011);
}
bool isWhite(char32_t c) {
    return c == U' ' || c == U'\t' || c == U'\n' || c == U'\r' || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A)
        || c == 0x202F || c == 0x205F || c == 0x3000;
}
bool isControl(char32_t c) {
    return (c < 32 && c != U'\t' && c != U'\n' && c != U'\r') || (c >= 0x7F && c < 0xA0) || c == 0xAD
        || (c >= 0x200B && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) || (c >= 0x2060 && c <= 0x2064) || c == 0xFEFF;
}

// The GPT-2 regex as the byte-level pre-tokenizer applies it:
// 's|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+
bool isL(char32_t c) {
    return isLetter(c) || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) || (c >= 0x370 && c <= 0x52F);
}
bool isN(char32_t c) { return isDigit(c) || (c >= 0xB2 && c <= 0xB3) || c == 0xB9 || (c >= 0xBC && c <= 0xBE); }
bool isS(char32_t c) { return isWhite(c) || c == 0x0B || c == 0x0C || c == 0x85 || c == 0x2028 || c == 0x2029; }

std::vector<std::u32string_view> gpt2Split(std::u32string_view text) {
    std::vector<std::u32string_view> out;
    const size_t n = text.size();
    size_t i = 0;
    auto run = [&](size_t from, auto test) { while (from < n && test(text[from])) ++from; return from; };
    while (i < n) {
        const char32_t c = text[i];
        if (c == U'\'' && i + 1 < n) {
            static const std::u32string_view tails[]{U"re", U"ve", U"ll", U"s", U"t", U"m", U"d"};
            bool matched = false;
            for (const auto tail : tails) {
                if (text.substr(i + 1, tail.size()) == tail) {
                    out.push_back(text.substr(i, 1 + tail.size()));
                    i += 1 + tail.size();
                    matched = true;
                    break;
                }
            }
            if (matched) continue;
        }
        const size_t body = (c == U' ' && i + 1 < n) ? i + 1 : i; // the optional leading space
        const char32_t b = text[body];
        if (body < n && (body == i || !isS(b))) {
            size_t end = 0;
            if (isL(b)) end = run(body, isL);
            else if (isN(b)) end = run(body, isN);
            else if (!isS(b)) end = run(body, [](char32_t x) { return !isS(x) && !isL(x) && !isN(x); });
            if (end > body) {
                out.push_back(text.substr(i, end - i));
                i = end;
                continue;
            }
        }
        // Whitespace: a run, leaving its last character to the word after it.
        size_t end = run(i, isS);
        if (end == i) end = i + 1; // (not reached: every character is one of the above)
        if (end < n && end - i > 1) --end;
        out.push_back(text.substr(i, end - i));
        i = end;
    }
    return out;
}

} // namespace

// ---- BERT WordPiece ---------------------------------------------------------------------

BertTokenizer::BertTokenizer(const std::filesystem::path& file) {
    const auto json = readJson(file);
    const auto& model = need(json["model"], "model");
    readVocab(model, vocab_);
    if (const auto* unk = model["unk_token"]) unk_ = idOf(vocab_, decodeUtf8(unk->text));
    if (const auto* most = model["max_input_chars_per_word"]) maxChars_ = size_t(most->number);
    cls_ = idOf(vocab_, U"[CLS]");
    sep_ = idOf(vocab_, U"[SEP]");
}

void BertTokenizer::piece(std::u32string_view word, BertEncoding& out) const {
    std::vector<int64_t> ids;
    if (word.size() <= maxChars_) {
        size_t start = 0;
        std::u32string candidate;
        while (start < word.size()) {
            int64_t found = -1;
            size_t end = word.size();
            for (; end > start; --end) {
                candidate.assign(start ? U"##" : U"");
                candidate.append(word.substr(start, end - start));
                if (const auto hit = vocab_.find(candidate); hit != vocab_.end()) { found = hit->second; break; }
            }
            if (found < 0) { ids.clear(); break; }
            ids.push_back(found);
            start = end;
        }
    }
    if (ids.empty()) ids.push_back(unk_);
    for (size_t k = 0; k < ids.size(); ++k) {
        out.ids.push_back(ids[k]);
        out.special.push_back(false);
        out.continues.push_back(k > 0);
    }
}

BertEncoding BertTokenizer::encode(std::u32string_view text) const {
    BertEncoding out;
    out.ids.push_back(cls_);
    out.special.push_back(true);
    out.continues.push_back(false);
    std::u32string word;
    auto flush = [&] { if (!word.empty()) piece(word, out); word.clear(); };
    for (const char32_t raw : text) {
        if (raw == 0 || raw == 0xFFFD || isControl(raw)) continue; // clean_text
        if (isWhite(raw)) { flush(); continue; }
        if (isBertPunctuation(raw)) { flush(); word.push_back(raw); flush(); continue; }
        word.push_back(raw);
    }
    flush();
    if (out.ids.size() > 511) { // truncation to 512, as the file asks
        out.ids.resize(511);
        out.special.resize(511);
        out.continues.resize(511);
    }
    out.ids.push_back(sep_);
    out.special.push_back(true);
    out.continues.push_back(false);
    return out;
}

// ---- RoBERTa byte-level BPE -------------------------------------------------------------

ByteBpeTokenizer::ByteBpeTokenizer(const std::filesystem::path& file) {
    const auto json = readJson(file);
    const auto& model = need(json["model"], "model");
    readVocab(model, vocab_);
    int rank = 0;
    for (const auto& merge : need(model["merges"], "merges").items) {
        std::u32string key;
        if (merge.kind == Json::Kind::String) key = decodeUtf8(merge.text);
        else if (merge.items.size() == 2) key = decodeUtf8(merge.items[0].text) + U" " + decodeUtf8(merge.items[1].text);
        ranks_.emplace(std::move(key), rank++);
    }
    if (const auto* added = json["added_tokens"]) {
        for (const auto& token : added->items) {
            auto content = decodeUtf8(need(token["content"], "content").text);
            longestAdded_ = std::max(longestAdded_, content.size());
            added_[std::move(content)] = int64_t(need(token["id"], "id").number);
        }
    }
    bos_ = idOf(vocab_.count(U"<s>") ? vocab_ : added_, U"<s>");
    eos_ = idOf(vocab_.count(U"</s>") ? vocab_ : added_, U"</s>");
    // GPT-2's printable stand-ins for the 256 byte values.
    int next = 0;
    for (int b = 0; b < 256; ++b) {
        const bool printable = (b >= '!' && b <= '~') || (b >= 0xA1 && b <= 0xAC) || (b >= 0xAE && b <= 0xFF);
        byteChar_[b] = printable ? char32_t(b) : char32_t(256 + next++);
    }
}

void ByteBpeTokenizer::word(std::string_view bytes, std::vector<int64_t>& out) const {
    std::vector<std::u32string> symbols;
    for (const unsigned char b : bytes) symbols.emplace_back(1, byteChar_[b]);
    std::u32string key;
    for (;;) {
        int best = std::numeric_limits<int>::max();
        size_t at = 0;
        for (size_t k = 0; k + 1 < symbols.size(); ++k) {
            key.assign(symbols[k]).append(U" ").append(symbols[k + 1]);
            if (const auto r = ranks_.find(key); r != ranks_.end() && r->second < best) { best = r->second; at = k; }
        }
        if (best == std::numeric_limits<int>::max()) break;
        // Every occurrence of the best pair, left to right.
        const std::u32string left = symbols[at], right = symbols[at + 1];
        std::vector<std::u32string> merged;
        for (size_t k = 0; k < symbols.size(); ++k) {
            if (k + 1 < symbols.size() && symbols[k] == left && symbols[k + 1] == right) {
                merged.push_back(left + right);
                ++k;
            } else {
                merged.push_back(std::move(symbols[k]));
            }
        }
        symbols = std::move(merged);
    }
    for (const auto& symbol : symbols) {
        const auto found = vocab_.find(symbol);
        if (found != vocab_.end()) out.push_back(found->second);
        else for (const char32_t c : symbol) { // (not reached with a complete vocabulary)
            const auto single = vocab_.find(std::u32string(1, c));
            if (single != vocab_.end()) out.push_back(single->second);
        }
    }
}

void ByteBpeTokenizer::sequence(std::u32string_view text, std::vector<int64_t>& out) const {
    auto plain = [&](std::u32string_view part) {
        for (const auto chunk : gpt2Split(part)) word(encodeUtf8(chunk), out);
    };
    // Added tokens first, leftmost and then longest, as the library splits on them.
    size_t from = 0, i = 0;
    std::u32string probe;
    while (i < text.size()) {
        int64_t id = -1;
        size_t length = 0;
        for (size_t len = std::min(longestAdded_, text.size() - i); len > 0; --len) {
            probe.assign(text.substr(i, len));
            if (const auto hit = added_.find(probe); hit != added_.end()) { id = hit->second; length = len; break; }
        }
        if (id < 0) { ++i; continue; }
        plain(text.substr(from, i - from));
        out.push_back(id);
        i += length;
        from = i;
    }
    plain(text.substr(from));
}

std::vector<int64_t> ByteBpeTokenizer::encode(std::u32string_view text) const {
    std::vector<int64_t> out;
    sequence(text, out);
    return out;
}

std::vector<int64_t> ByteBpeTokenizer::encodePair(std::u32string_view a, std::u32string_view b) const {
    std::vector<int64_t> out{bos_};
    sequence(a, out);
    out.push_back(eos_);
    out.push_back(eos_);
    sequence(b, out);
    out.push_back(eos_);
    if (out.size() > 512) { // (a subtitle line is far shorter)
        out.resize(511);
        out.push_back(eos_);
    }
    return out;
}

} // namespace veyra::tts
