// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Numbers as words: the voice model knows letters only (its alphabet has no digits).
// Cardinal numbers in the nominative, which is how a number written in a subtitle is
// usually read out; decimals as "три целых четырнадцать сотых" / "three point one four".
#include <string>
#include <string_view>

namespace veyra::tts {

// `digits`: an integer or a decimal ("14", "3.5", "3,5"), at most 15 digits before the
// separator; anything longer is read digit by digit.
// `feminine`: "одна", "две" for a number that counts a feminine noun ("две минуты").
std::u32string russianNumber(std::u32string_view digits, bool feminine = false);
std::u32string englishNumber(std::u32string_view digits);

// Every run of digits in `text` (with a decimal separator between digits) replaced by words.
std::u32string spellNumbers(std::u32string_view text, bool russian);

} // namespace veyra::tts
