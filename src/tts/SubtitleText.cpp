// SPDX-License-Identifier: GPL-3.0-only
#include "SubtitleText.h"
#include "Utf8.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <optional>
#include <vector>

namespace veyra::tts {
namespace {

constexpr double memorySeconds = 20.0;

std::u32string collapse(std::u32string_view text) {
    std::u32string out;
    for (const char32_t c : text) {
        if (isSpace(c)) {
            if (!out.empty() && out.back() != U' ') out.push_back(U' ');
        } else {
            out.push_back(c);
        }
    }
    while (!out.empty() && out.back() == U' ') out.pop_back();
    return out;
}

std::vector<std::u32string> words(std::u32string_view text) {
    std::vector<std::u32string> out;
    std::u32string current;
    for (const char32_t c : text) {
        if (isSpace(c)) {
            if (!current.empty()) out.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) out.push_back(std::move(current));
    return out;
}

bool hasLetter(std::u32string_view text) { return std::any_of(text.begin(), text.end(), isLetter); }

// A character's name: one to three words, the first beginning with a capital, no sentence
// marks or digits, not too long: "Анна", "Офицер Браун", and a description of the speaker
// as games write it, "Незнакомый человек", "Старый моряк".
bool looksLikeName(std::u32string_view name) {
    const auto list = words(name);
    if (list.empty() || list.size() > 3 || name.size() > 30 || !hasLetter(name)) return false;
    for (const char32_t c : name)
        if (c == U'.' || c == U'!' || c == U'?' || c == U'…' || c == U',' || c == U';' || isDigit(c)) return false;
    const auto& first = list.front();
    return !first.empty() && (!isLetter(first[0]) || isUpper(first[0]));
}

// "[Смеётся]", "[Музыка]": a bracketed word that describes a sound or an action is no name.
bool describesSound(std::u32string_view name) {
    const auto list = words(name);
    if (list.empty()) return false;
    const auto last = toLower(list.back());
    // ("-ат" is left out: "[Солдат]" names a speaker.)
    static const std::array<std::u32string_view, 11> verbEndings{U"ся", U"сь", U"ет", U"ёт", U"ит", U"ут",
                                                                 U"ют", U"ят", U"ом", U"ем", U"ой"};
    for (const auto ending : verbEndings)
        if (last.size() > ending.size() + 2 && last.ends_with(ending)) return true;
    static const std::array<std::u32string_view, 16> sounds{U"музыка", U"смех", U"тишина", U"выстрел", U"выстрелы",
        U"взрыв", U"шум", U"крик", U"крики", U"стон", U"плач", U"вздох", U"аплодисменты", U"звонок", U"гудок", U"стук"};
    return std::find(sounds.begin(), sounds.end(), last) != sounds.end();
}

std::u32string lowered(std::u32string_view text) {
    std::u32string out;
    for (const char32_t c : text) out.push_back(c == U'Ё' || c == U'ё' ? U'е' : toLower(c));
    return out;
}

bool startsWithAny(const std::u32string& word, std::initializer_list<std::u32string_view> stems) {
    return std::any_of(stems.begin(), stems.end(), [&](std::u32string_view stem) { return word.starts_with(stem); });
}

bool allCaps(std::u32string_view text) {
    return hasLetter(text) && std::none_of(text.begin(), text.end(), [](char32_t c) { return isLetter(c) && !isUpper(c); });
}

std::u32string trimName(std::u32string_view name) {
    std::u32string out = collapse(name);
    while (!out.empty() && (out.back() == U':' || out.back() == U'：')) out.pop_back();
    return collapse(out);
}

// "Name: text", "NAME: text", "[Name] text", "(Name) text".
void splitSpeaker(std::u32string& text, std::u32string& speaker) {
    if (!text.empty() && (text[0] == U'[' || text[0] == U'(')) {
        const char32_t closing = text[0] == U'[' ? U']' : U')';
        const auto end = text.find(closing);
        if (end != std::u32string::npos && end > 1) {
            const auto name = trimName(text.substr(1, end - 1));
            const auto rest = collapse(text.substr(end + 1));
            if (looksLikeName(name) && !describesSound(name) && hasLetter(rest)) {
                speaker = name;
                text = rest;
                return;
            }
        }
    }
    const auto colon = text.find_first_of(U":：");
    if (colon == std::u32string::npos || colon == 0 || colon > 31) return;
    // "10:30", "http://": no space after the colon. The OCR also glues a name to the line when
    // the gap is small ("Лусия:Да ну тебя."): a capital letter right after it still splits.
    if (colon + 1 < text.size() && !isSpace(text[colon + 1]) && !(isLetter(text[colon + 1]) && isUpper(text[colon + 1])))
        return;
    const auto name = trimName(text.substr(0, colon));
    const auto rest = collapse(text.substr(colon + 1));
    // "Незнакомый человек:" alone: the name stands over the line that follows (nothing to say).
    if (!hasLetter(rest) && looksLikeName(name) && !describesSound(name)) {
        speaker = name;
        text.clear();
        return;
    }
    // "Слушай: это важно." is speech: after a name the line begins with a capital letter
    // (or the name stands in capitals, "ТРЕВОР: ...").
    const auto first = std::find_if(rest.begin(), rest.end(), isLetter);
    const bool capital = first != rest.end() && isUpper(*first);
    // (A speaker described, "Незнакомый человек: эй, ты!", is a name whatever follows.)
    if (looksLikeName(name) && (capital || allCaps(name) || describedSpeaker(name)) && hasLetter(rest)) {
        speaker = name;
        text = rest;
    }
}

// What stands in square brackets describes a sound, it is never speech; a bracket whose
// closing one was not read takes the rest of the line (better silent than read aloud).
std::u32string withoutBracketed(std::u32string_view text) {
    std::u32string out;
    int depth = 0;
    for (const char32_t c : text) {
        if (c == U'[' || c == U'【') { ++depth; out.push_back(U' '); continue; }
        if (c == U']' || c == U'】') { if (depth) --depth; continue; }
        if (c == U'♪' || c == U'♫' || c == U'*' || c == U'#') { out.push_back(U' '); continue; }
        if (!depth) out.push_back(c);
    }
    return out;
}

// Dialogue dashes at the start of a line and of each speaker's part: "- Где он? - Ушёл."
std::u32string withoutDialogueDashes(std::u32string_view text) {
    std::u32string out;
    for (size_t i = 0; i < text.size(); ++i) {
        const char32_t c = text[i];
        const bool dash = c == U'-' || c == U'—' || c == U'–';
        const bool startsPart = out.empty() || out.back() == U' ';
        const bool beforeWord = i + 1 < text.size() && (isSpace(text[i + 1]) || isLetter(text[i + 1]));
        if (dash && startsPart && beforeWord && (out.empty() || out.size() < 2 ||
            out[out.size() - 2] == U'.' || out[out.size() - 2] == U'!' || out[out.size() - 2] == U'?' ||
            out[out.size() - 2] == U'…')) continue;
        out.push_back(c);
    }
    return out;
}

constexpr std::u32string_view latinLookalikes = U"aceopxyABCEHKMOPTX";
constexpr std::u32string_view cyrillicTwins   = U"асеорхуАВСЕНКМОРТХ";

// Latin letters inside a word with Cyrillic ones are misread Cyrillic letters - when every
// one of them has a Cyrillic twin ("Пpивeт"); a word with a Latin letter like no Cyrillic one
// ("Phoenix" read partly as Cyrillic) is left alone.
std::u32string repairLookalikes(std::u32string_view text) {
    std::u32string out(text);
    for (size_t i = 0; i < out.size();) {
        if (!isLetter(out[i])) { ++i; continue; }
        size_t end = i;
        bool cyrillic = false, latin = false, allTwins = true;
        for (; end < out.size() && isLetter(out[end]); ++end) {
            if (isCyrillic(out[end])) cyrillic = true;
            else {
                latin = true;
                if (latinLookalikes.find(out[end]) == std::u32string_view::npos) allTwins = false;
            }
        }
        if (cyrillic && latin && allTwins)
            for (size_t k = i; k < end; ++k)
                if (const auto at = latinLookalikes.find(out[k]); at != std::u32string_view::npos) out[k] = cyrillicTwins[at];
        i = end;
    }
    return out;
}

// Lower case, ё as е, everything but letters and digits as single spaces.
std::u32string plain(std::u32string_view text) {
    std::u32string out;
    for (const char32_t c : text) {
        char32_t l = toLower(c);
        if (l == U'ё') l = U'е';
        if (isLetter(l) || isDigit(l)) out.push_back(l);
        else if (!out.empty() && out.back() != U' ') out.push_back(U' ');
    }
    while (!out.empty() && out.back() == U' ') out.pop_back();
    return out;
}

// How alike two plain strings are: 1 - edit distance / longer length.
double likeness(std::u32string_view a, std::u32string_view b) {
    if (a.empty() || b.empty()) return a == b ? 1.0 : 0.0;
    std::vector<size_t> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        size_t diagonal = row[0];
        row[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            const size_t up = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diagonal = up;
        }
    }
    return 1.0 - double(row[b.size()]) / double(std::max(a.size(), b.size()));
}

} // namespace

SubtitleLine cleanSubtitle(std::u32string_view line, bool names) {
    SubtitleLine out;
    out.text = collapse(line);
    if (names) splitSpeaker(out.text, out.speaker); // before brackets: "[Name] text" names the speaker
    out.text = collapse(withoutDialogueDashes(collapse(withoutBracketed(out.text))));
    out.text = repairLookalikes(out.text);
    if (!out.speaker.empty()) out.speaker = repairLookalikes(out.speaker);
    if (!hasLetter(out.text)) out.text.clear();
    return out;
}

bool describedSpeaker(std::u32string_view name, std::optional<bool>* female) {
    const auto list = words(name);
    if (list.empty() || list.size() > 3) return false;
    // A proper name among the words makes a person: "Офицер Браун", "Агент Смит".
    for (size_t i = 1; i < list.size(); ++i)
        if (!list[i].empty() && isLetter(list[i][0]) && isUpper(list[i][0]) && !allCaps(name)) return false;
    static const std::initializer_list<std::u32string_view> women{U"женщин", U"девушк", U"девочк", U"незнакомк",
        U"продавщиц", U"официантк", U"медсестр", U"пассажирк", U"клиентк", U"посетительниц", U"жительниц",
        U"охранниц", U"бабушк", U"старушк", U"тетк", U"тетя", U"дама", U"леди", U"мама", U"мать"};
    static const std::initializer_list<std::u32string_view> men{U"мужчин", U"парен", U"парн", U"мальчик",
        U"незнакомец", U"дедушк", U"старик", U"дядь", U"мужик", U"отец", U"папа"};
    static const std::initializer_list<std::u32string_view> anyone{U"незнаком", U"неизвестн", U"прохож", U"человек",
        U"голос", U"толп", U"охранник", U"полицейск", U"коп", U"водител", U"продавец", U"бармен", U"официант",
        U"солдат", U"рабоч", U"бандит", U"гангстер", U"житель", U"пешеход", U"клиент", U"посетител", U"диктор",
        U"ведущ", U"репортер", U"диспетчер", U"санитар", U"пассажир", U"байкер", U"бездомн", U"турист", U"охотник",
        U"работник", U"сотрудник", U"громил", U"наемник", U"сосед", U"ребенок",
        U"кто-то", U"рация", U"радио", U"оператор", U"механик", U"таксист", U"бродяг", U"пьяниц", U"фанат"};
    bool described = false;
    std::optional<bool> sex;
    for (const auto& w : list) {
        const auto l = lowered(w);
        if (startsWithAny(l, women)) { described = true; sex = true; }
        else if (startsWithAny(l, men)) { described = true; if (!sex) sex = false; }
        else if (startsWithAny(l, anyone)) described = true;
    }
    if (!described) return false;
    // "Пожилая женщина", "Незнакомая": the adjective says it too.
    const auto firstWord = lowered(list.front());
    if (!sex && firstWord.size() > 3 && (firstWord.ends_with(U"ая") || firstWord.ends_with(U"яя"))) sex = true;
    if (female) *female = sex;
    return true;
}

bool nameLooksFemale(std::u32string_view name) {
    const auto list = words(name);
    if (list.empty()) return false;
    const auto last = toLower(list.back());
    static const std::array<std::u32string_view, 14> maleInA{U"никита", U"илья", U"саша", U"миша", U"лёша",
        U"леша", U"дима", U"ваня", U"петя", U"лука", U"фома", U"кузьма", U"гоша", U"паша"};
    if (std::find(maleInA.begin(), maleInA.end(), last) != maleInA.end()) return false;
    return !last.empty() && (last.back() == U'а' || last.back() == U'я');
}

std::u32string SpokenMemory::unsaid(std::u32string_view text, double now) {
    while (!recent_.empty() && now - recent_.front().at > memorySeconds) recent_.pop_front();
    const auto line = plain(text);
    if (line.empty()) return {};
    std::u32string result(text);
    for (auto said = recent_.rbegin(); said != recent_.rend(); ++said) {
        if (line == said->plain || likeness(line, said->plain) >= 0.88) return {};
        if (said->plain.size() >= 8 && line.starts_with(said->plain)) {
            // The subtitle kept the sentence said before and added to it: only the new words.
            const auto all = words(text);
            std::u32string rest;
            for (size_t i = said->words; i < all.size(); ++i) {
                if (!rest.empty()) rest.push_back(U' ');
                rest += all[i];
            }
            result = rest;
            break;
        }
    }
    recent_.push_back({now, line, words(text).size()});
    while (recent_.size() > 16) recent_.pop_front();
    return hasLetter(result) ? result : std::u32string{};
}

// (0.8: one letter wrong in a five-letter name.)
const std::u32string* NameBook::known(std::u32string_view name) const {
    const auto wanted = plain(name);
    if (wanted.empty()) return nullptr;
    for (const auto& seen : names_) {
        const auto other = plain(seen);
        if (other == wanted || likeness(wanted, other) >= 0.8) return &seen;
    }
    return nullptr;
}

std::u32string NameBook::canonical(std::u32string_view name) {
    if (const auto* seen = known(name)) return *seen;
    names_.emplace_back(name);
    if (names_.size() > 64) names_.erase(names_.begin());
    return std::u32string(name);
}

void NameBook::knownPrefix(SubtitleLine& line) const {
    if (!line.speaker.empty()) return;
    const auto list = words(line.text);
    for (size_t size = 3; size >= 1; --size) {
        if (list.size() <= size) continue;
        std::u32string head;
        for (size_t i = 0; i < size; ++i) head += (i ? U" " : U"") + list[i];
        const char32_t mark = head.back();
        if (mark != U';' && mark != U'|' && mark != U':' && mark != U'：') continue;
        head.pop_back();
        const auto* seen = known(head);
        if (seen || describedSpeaker(head)) {
            std::u32string rest;
            for (size_t i = size; i < list.size(); ++i) rest += (rest.empty() ? U"" : U" ") + list[i];
            line.speaker = seen ? *seen : head;
            line.text = rest;
            return;
        }
    }
}

} // namespace veyra::tts
