// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// The few pieces of Unicode the voice needs: UTF-8 <-> code points, Russian and Latin
// letters, lower case. Portable C++ (no Qt, no Windows) so the text side is testable anywhere.
#include <cstdint>
#include <string>
#include <string_view>

namespace veyra::tts {

inline std::u32string decodeUtf8(std::string_view text) {
    std::u32string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const auto c = uint8_t(text[i]);
        char32_t value = 0;
        int extra = 0;
        if (c < 0x80) { value = c; }
        else if ((c >> 5) == 6) { value = c & 0x1F; extra = 1; }
        else if ((c >> 4) == 14) { value = c & 0x0F; extra = 2; }
        else if ((c >> 3) == 30) { value = c & 0x07; extra = 3; }
        else { ++i; continue; } // a stray continuation byte
        if (extra && i + extra >= text.size()) break; // cut off in the middle of a character
        bool valid = true;
        for (int k = 1; k <= extra; ++k) {
            const auto next = uint8_t(text[i + k]);
            if ((next >> 6) != 2) { valid = false; break; }
            value = (value << 6) | (next & 0x3F);
        }
        i += valid ? extra + 1 : 1;
        if (valid) out.push_back(value);
    }
    return out;
}

inline void appendUtf8(std::string& out, char32_t c) {
    if (c < 0x80) out.push_back(char(c));
    else if (c < 0x800) { out.push_back(char(0xC0 | (c >> 6))); out.push_back(char(0x80 | (c & 0x3F))); }
    else if (c < 0x10000) {
        out.push_back(char(0xE0 | (c >> 12)));
        out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(char(0x80 | (c & 0x3F)));
    } else {
        out.push_back(char(0xF0 | (c >> 18)));
        out.push_back(char(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(char(0x80 | (c & 0x3F)));
    }
}

inline std::string encodeUtf8(std::u32string_view text) {
    std::string out;
    out.reserve(text.size() * 2);
    for (const char32_t c : text) appendUtf8(out, c);
    return out;
}

inline bool isCyrillic(char32_t c) { return (c >= U'А' && c <= U'я') || c == U'Ё' || c == U'ё'; }
inline bool isLatin(char32_t c) { return (c >= U'A' && c <= U'Z') || (c >= U'a' && c <= U'z'); }
inline bool isDigit(char32_t c) { return c >= U'0' && c <= U'9'; }
inline bool isLetter(char32_t c) { return isCyrillic(c) || isLatin(c); }
inline bool isSpace(char32_t c) { return c == U' ' || c == U'\t' || c == U'\n' || c == U'\r' || c == 0xA0; }
inline bool isUpper(char32_t c) { return (c >= U'А' && c <= U'Я') || c == U'Ё' || (c >= U'A' && c <= U'Z'); }

inline char32_t toLower(char32_t c) {
    if (c >= U'А' && c <= U'Я') return c + 32;
    if (c == U'Ё') return U'ё';
    if (c >= U'A' && c <= U'Z') return c + 32;
    return c;
}
inline char32_t toUpper(char32_t c) {
    if (c >= U'а' && c <= U'я') return c - 32;
    if (c == U'ё') return U'Ё';
    if (c >= U'a' && c <= U'z') return c - 32;
    return c;
}
inline std::u32string toLower(std::u32string_view text) {
    std::u32string out(text);
    for (auto& c : out) c = toLower(c);
    return out;
}

inline bool isRussianVowel(char32_t c) {
    switch (toLower(c)) {
    case U'а': case U'е': case U'ё': case U'и': case U'о': case U'у': case U'ы': case U'э': case U'ю': case U'я':
        return true;
    default:
        return false;
    }
}

// 64-bit FNV-1a of the code points: the key a word is looked up by in the stress dictionary.
inline uint64_t wordHash(std::u32string_view word) {
    uint64_t hash = 1469598103934665603ull;
    for (const char32_t c : word) {
        for (int shift = 0; shift < 32; shift += 8) {
            hash ^= uint64_t((c >> shift) & 0xFF);
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

} // namespace veyra::tts
