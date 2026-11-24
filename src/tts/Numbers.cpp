// SPDX-License-Identifier: GPL-3.0-only
#include "Numbers.h"
#include "Utf8.h"

#include <array>
#include <cstdint>

namespace veyra::tts {
namespace {

std::u32string u(std::string_view utf8) { return decodeUtf8(utf8); }

void join(std::u32string& out, const std::u32string& word) {
    if (word.empty()) return;
    if (!out.empty()) out.push_back(U' ');
    out += word;
}

// --- Russian -------------------------------------------------------------------------

// 0..999 in words; `feminine`: "одна", "две" (тысяча is feminine).
std::u32string russianHundreds(int n, bool feminine) {
    static const std::array<const char*, 10> hundreds{"", "сто", "двести", "триста", "четыреста", "пятьсот",
                                                       "шестьсот", "семьсот", "восемьсот", "девятьсот"};
    static const std::array<const char*, 10> tens{"", "", "двадцать", "тридцать", "сорок", "пятьдесят",
                                                   "шестьдесят", "семьдесят", "восемьдесят", "девяносто"};
    static const std::array<const char*, 20> ones{"", "один", "два", "три", "четыре", "пять", "шесть", "семь",
                                                   "восемь", "девять", "десять", "одиннадцать", "двенадцать",
                                                   "тринадцать", "четырнадцать", "пятнадцать", "шестнадцать",
                                                   "семнадцать", "восемнадцать", "девятнадцать"};
    std::u32string out;
    join(out, u(hundreds[n / 100]));
    const int rest = n % 100;
    if (rest >= 20) {
        join(out, u(tens[rest / 10]));
        const int one = rest % 10;
        if (one == 1 && feminine) join(out, u("одна"));
        else if (one == 2 && feminine) join(out, u("две"));
        else join(out, u(ones[one]));
    } else if (rest == 1 && feminine) {
        join(out, u("одна"));
    } else if (rest == 2 && feminine) {
        join(out, u("две"));
    } else {
        join(out, u(ones[rest]));
    }
    return out;
}

// The form of a noun after n: 1 -> one, 2..4 -> few, else many ("тысяча", "тысячи", "тысяч").
const char* plural(uint64_t n, const char* one, const char* few, const char* many) {
    const auto last2 = n % 100, last = n % 10;
    if (last2 >= 11 && last2 <= 14) return many;
    if (last == 1) return one;
    if (last >= 2 && last <= 4) return few;
    return many;
}

std::u32string russianInteger(uint64_t n, bool feminine = false) {
    if (n == 0) return u("ноль");
    struct Scale { uint64_t value; const char *one, *few, *many; bool feminine; };
    static const std::array<Scale, 4> scales{{
        {1000000000000ull, "триллион", "триллиона", "триллионов", false},
        {1000000000ull, "миллиард", "миллиарда", "миллиардов", false},
        {1000000ull, "миллион", "миллиона", "миллионов", false},
        {1000ull, "тысяча", "тысячи", "тысяч", true},
    }};
    std::u32string out;
    for (const auto& scale : scales) {
        const auto count = n / scale.value;
        n %= scale.value;
        if (!count) continue;
        join(out, russianHundreds(int(count), scale.feminine));
        join(out, u(plural(count, scale.one, scale.few, scale.many)));
    }
    join(out, russianHundreds(int(n), feminine));
    return out;
}

// --- English -------------------------------------------------------------------------

std::u32string englishHundreds(int n) {
    static const std::array<const char*, 20> ones{"", "one", "two", "three", "four", "five", "six", "seven",
                                                   "eight", "nine", "ten", "eleven", "twelve", "thirteen",
                                                   "fourteen", "fifteen", "sixteen", "seventeen", "eighteen",
                                                   "nineteen"};
    static const std::array<const char*, 10> tens{"", "", "twenty", "thirty", "forty", "fifty", "sixty",
                                                   "seventy", "eighty", "ninety"};
    std::u32string out;
    if (n >= 100) {
        join(out, u(ones[n / 100]));
        join(out, u("hundred"));
    }
    const int rest = n % 100;
    if (rest >= 20) {
        std::string word = tens[rest / 10];
        if (rest % 10) word += std::string("-") + ones[rest % 10];
        join(out, u(word));
    } else {
        join(out, u(ones[rest]));
    }
    return out;
}

std::u32string englishInteger(uint64_t n) {
    if (n == 0) return u("zero");
    struct Scale { uint64_t value; const char* name; };
    static const std::array<Scale, 4> scales{{{1000000000000ull, "trillion"}, {1000000000ull, "billion"},
                                              {1000000ull, "million"}, {1000ull, "thousand"}}};
    std::u32string out;
    for (const auto& scale : scales) {
        const auto count = n / scale.value;
        n %= scale.value;
        if (!count) continue;
        join(out, englishHundreds(int(count)));
        join(out, u(scale.name));
    }
    join(out, englishHundreds(int(n)));
    return out;
}

bool parse(std::u32string_view digits, uint64_t& value) {
    if (digits.empty() || digits.size() > 15) return false;
    value = 0;
    for (const char32_t c : digits) {
        if (!isDigit(c)) return false;
        value = value * 10 + uint64_t(c - U'0');
    }
    return true;
}

std::u32string digitByDigit(std::u32string_view digits, bool russian) {
    std::u32string out;
    for (const char32_t c : digits)
        if (isDigit(c)) join(out, russian ? russianInteger(uint64_t(c - U'0')) : englishInteger(uint64_t(c - U'0')));
    return out;
}

// Whether the integer text[begin, end) counts a feminine noun, judged by the word after it:
// after 1 a feminine noun ends in а/я ("одна минута"), after 2 in ы/и ("две машины"), where a
// masculine or neuter one would end otherwise ("один патрон", "два патрона", "два окна").
bool countsFeminine(std::u32string_view text, size_t begin, size_t end) {
    const auto last2 = end - begin >= 2 ? (text[end - 2] - U'0') * 10 + (text[end - 1] - U'0') : 0;
    const int last = int(text[end - 1] - U'0');
    if ((last != 1 && last != 2) || (last2 >= 11 && last2 <= 12)) return false;
    if (end < text.size() && (text[end] == U'.' || text[end] == U',') && end + 1 < text.size() && isDigit(text[end + 1]))
        return false; // a decimal
    size_t at = end;
    while (at < text.size() && isSpace(text[at])) ++at;
    size_t wordEnd = at;
    while (wordEnd < text.size() && isCyrillic(text[wordEnd])) ++wordEnd;
    if (wordEnd - at < 2) return false;
    const char32_t ending = toLower(text[wordEnd - 1]);
    return last == 1 ? (ending == U'а' || ending == U'я') : (ending == U'ы' || ending == U'и');
}

} // namespace

std::u32string russianNumber(std::u32string_view digits, bool feminine) {
    const auto separator = digits.find_first_of(U".,");
    const auto whole = digits.substr(0, separator);
    uint64_t integer = 0;
    if (!parse(whole, integer)) return digitByDigit(digits, true);
    if (separator == std::u32string_view::npos) return russianInteger(integer, feminine);
    auto fraction = digits.substr(separator + 1);
    uint64_t part = 0;
    if (fraction.size() > 3 || !parse(fraction, part)) {
        auto out = russianInteger(integer);
        join(out, u("точка"));
        join(out, digitByDigit(fraction, true));
        return out;
    }
    // "две целых пять десятых": the fraction's denominator agrees with its numerator.
    auto out = russianInteger(integer, true);
    join(out, u(plural(integer, "целая", "целых", "целых")));
    join(out, russianInteger(part, true));
    switch (fraction.size()) {
    case 1: join(out, u(plural(part, "десятая", "десятых", "десятых"))); break;
    case 2: join(out, u(plural(part, "сотая", "сотых", "сотых"))); break;
    default: join(out, u(plural(part, "тысячная", "тысячных", "тысячных"))); break;
    }
    return out;
}

std::u32string englishNumber(std::u32string_view digits) {
    const auto separator = digits.find_first_of(U".,");
    uint64_t integer = 0;
    if (!parse(digits.substr(0, separator), integer)) return digitByDigit(digits, false);
    auto out = englishInteger(integer);
    if (separator != std::u32string_view::npos) {
        join(out, u("point"));
        join(out, digitByDigit(digits.substr(separator + 1), false));
    }
    return out;
}

std::u32string spellNumbers(std::u32string_view text, bool russian) {
    std::u32string out;
    out.reserve(text.size() * 2);
    for (size_t i = 0; i < text.size();) {
        if (!isDigit(text[i])) {
            out.push_back(text[i++]);
            continue;
        }
        size_t end = i;
        while (end < text.size() && isDigit(text[end])) ++end;
        // A decimal separator counts only between digits ("3.5"), not at a sentence end ("14.").
        if (end + 1 < text.size() && (text[end] == U'.' || text[end] == U',') && isDigit(text[end + 1])) {
            ++end;
            while (end < text.size() && isDigit(text[end])) ++end;
        }
        const auto words = russian ? russianNumber(text.substr(i, end - i), countsFeminine(text, i, end))
                                   : englishNumber(text.substr(i, end - i));
        // Keep words apart from letters glued to the number ("5км" -> "пять км").
        if (!out.empty() && isLetter(out.back())) out.push_back(U' ');
        out += words;
        if (end < text.size() && isLetter(text[end])) out.push_back(U' ');
        i = end;
    }
    return out;
}

} // namespace veyra::tts
