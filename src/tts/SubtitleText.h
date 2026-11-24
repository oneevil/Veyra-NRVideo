// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// A subtitle line as it should be spoken: the speaker's name taken off ("Анна: ...",
// "[Анна] ..."), descriptions of sounds dropped ("[Смеётся]", "[Музыка]"), dialogue dashes
// and note signs removed, and Latin look-alike letters that OCR put inside Russian words
// turned back into Cyrillic ("Пpивeт" with Latin p and e would be read as English).
// Portable C++, testable without Windows.
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace veyra::tts {

struct SubtitleLine {
    std::u32string speaker; // empty: no name shown with the line
    std::u32string text;    // what to say; empty: nothing to say
};

// `names`: whether a name may stand with the line (false: a game whose subtitles never name
// the speaker, where "Внимание: ..." is all to be read).
SubtitleLine cleanSubtitle(std::u32string_view line, bool names = true);

// A rough guess from a Russian name's ending, to pick a man's or a woman's narrator.
bool nameLooksFemale(std::u32string_view name);

// A speaker described rather than named - "Незнакомый человек", "Прохожий", "Женщина", "Коп":
// a different person every time, so no character of their own (one voice and one sex for all
// of them would read a woman with a man's narrator). `female`, when given, is set from the
// words when they say it ("Незнакомка", "Пожилая женщина"; empty for "Незнакомый человек").
bool describedSpeaker(std::u32string_view name, std::optional<bool>* female = nullptr);

// What of a new line has not been said yet: nothing when it repeats a recent line (OCR read
// the same subtitle again after a flicker), only the added words when a subtitle kept its
// earlier sentence and grew.
class SpokenMemory {
public:
    std::u32string unsaid(std::u32string_view text, double nowSeconds);
    void clear() { recent_.clear(); }

private:
    struct Said { double at; std::u32string plain; size_t words; };
    std::deque<Said> recent_;
};

// The characters met so far, kept so that one character is one speaker:
// a name read slightly wrong ("Лусия", "ЛЮСИЯ") is the character already seen ("Люсия"), and a
// line beginning with a known name whose colon OCR misread ("Люсия; Да") still names it.
class NameBook {
public:
    // The name as first seen, when this one is that name misread; otherwise it is new.
    std::u32string canonical(std::u32string_view name);
    // A line with no name found that begins with a known one followed by ";" or "|" instead
    // of a colon - or by a colon and a small letter ("Незнакомый человек: эй, ты!") - or with a
    // speaker described that way: the name is taken off. (Not after a comma or "!": "Люсия, иди
    // сюда" calls her.)
    void knownPrefix(SubtitleLine& line) const;
    void clear() { names_.clear(); }

private:
    const std::u32string* known(std::u32string_view name) const;
    std::vector<std::u32string> names_;
};

} // namespace veyra::tts
