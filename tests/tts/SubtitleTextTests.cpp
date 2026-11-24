// SPDX-License-Identifier: GPL-3.0-only
// How subtitle lines are prepared for the voice (src/tts/SubtitleText): names, sound
// descriptions, dialogue dashes, OCR look-alike letters, repeated and growing lines.
#include "../../src/tts/SubtitleText.h"
#include "../../src/tts/Utf8.h"

#include <cstdio>
#include <optional>

using namespace veyra::tts;

namespace {
int failures = 0;
void expect(const char* input, const char* speaker, const char* text) {
    const auto line = cleanSubtitle(decodeUtf8(input));
    const auto gotSpeaker = encodeUtf8(line.speaker), gotText = encodeUtf8(line.text);
    if (gotSpeaker != speaker || gotText != text) {
        ++failures;
        std::printf("FAIL %s\n  speaker [%s] want [%s]\n  text [%s] want [%s]\n", input, gotSpeaker.c_str(), speaker,
                    gotText.c_str(), text);
    }
}
void expectUnsaid(SpokenMemory& memory, const char* input, double at, const char* want) {
    const auto got = encodeUtf8(memory.unsaid(decodeUtf8(input), at));
    if (got != want) {
        ++failures;
        std::printf("FAIL unsaid %s -> [%s] want [%s]\n", input, got.c_str(), want);
    }
}
}

int main() {
    expect("Анна: Где ты был всю ночь?", "Анна", "Где ты был всю ночь?");
    expect("ТРЕВОР: Заводи машину!", "ТРЕВОР", "Заводи машину!");
    expect("[Офицер Браун] Руки вверх!", "Офицер Браун", "Руки вверх!");
    expect("[Смеётся] Ну ты даёшь.", "", "Ну ты даёшь.");
    expect("[Солдат] Стоять!", "Солдат", "Стоять!");
    expect("[шёпотом] Тише.", "", "Тише.");
    expect("Ну ты даёшь. [музыка]", "", "Ну ты даёшь.");
    expect("[Музыка]", "", "");
    expect("- Где он? - Ушёл.", "", "Где он? Ушёл.");
    expect("Он - врач, а не солдат.", "", "Он - врач, а не солдат.");
    expect("♪ Солнце светит ♪", "", "Солнце светит");
    expect("Встреча в 10:30 у моста.", "", "Встреча в 10:30 у моста.");
    expect("Слушай: это важно.", "", "Слушай: это важно.");
    expect("Незнакомый человек: Ты кто такой?", "Незнакомый человек", "Ты кто такой?");
    expect("Старый моряк: Шторм идёт.", "Старый моряк", "Шторм идёт.");
    expect("Незнакомый человек:", "Незнакомый человек", "");
    expect("НЕЗНАКОМЫЙ ЧЕЛОВЕК: Стой!", "НЕЗНАКОМЫЙ ЧЕЛОВЕК", "Стой!");
    expect("Итак:", "Итак", "");
    expect("Лусия:Да ну тебя.", "Лусия", "Да ну тебя.");
    expect("Лусия : Да ну тебя.", "Лусия", "Да ну тебя.");
    expect("Джейсон: До сих пор не могу поверить, что я сошелся с угонщицей.", "Джейсон",
           "До сих пор не могу поверить, что я сошелся с угонщицей.");
    expect("Сайт http://example.com работает.", "", "Сайт http://example.com работает.");
    expect("Счёт 2:1 в нашу пользу.", "", "Счёт 2:1 в нашу пользу.");
    expect("Цена: 100 долларов.", "", "Цена: 100 долларов.");
    expect("Пpивeт, кaк дeлa?", "", "Привет, как дела?");
    expect("Найди Phoenix на стоянке.", "", "Найди Phoenix на стоянке.");
    expect("  Несколько   пробелов\nи строк  ", "", "Несколько пробелов и строк");
    expect("Я не знаю [незакрытая скобка", "", "Я не знаю");
    // A speaker described is a name also before a small letter (field report: it was read out).
    expect("Незнакомый человек: эй, ты!", "Незнакомый человек", "эй, ты!");
    expect("Прохожий: да отстань.", "Прохожий", "да отстань.");
    expect("Слушай: это важно.", "", "Слушай: это важно.");

    {
        std::optional<bool> sex;
        const auto described = [&](const char32_t* n, bool want, std::optional<bool> wantSex) {
            sex.reset();
            if (describedSpeaker(n, &sex) != want || (want && sex != wantSex)) {
                ++failures;
                std::printf("FAIL describedSpeaker %s\n", encodeUtf8(n).c_str());
            }
        };
        described(U"Незнакомый человек", true, std::nullopt);
        described(U"НЕЗНАКОМЫЙ ЧЕЛОВЕК", true, std::nullopt);
        described(U"Незнакомка", true, true);
        described(U"Пожилая женщина", true, true);
        described(U"Мужчина", true, false);
        described(U"Коп", true, std::nullopt);
        described(U"Офицер Браун", false, std::nullopt);
        described(U"Люсия", false, std::nullopt);
        described(U"Джейсон", false, std::nullopt);
    }

    if (!nameLooksFemale(U"Анна") || nameLooksFemale(U"Тревор") || nameLooksFemale(U"Никита") ||
        !nameLooksFemale(U"Мария Петрова")) {
        ++failures;
        std::printf("FAIL nameLooksFemale\n");
    }

    SpokenMemory memory;
    expectUnsaid(memory, "Мы должны уйти.", 0.0, "Мы должны уйти.");
    expectUnsaid(memory, "Мы должны уйти.", 1.0, "");                       // read again
    expectUnsaid(memory, "Мы должны yйти.", 2.0, "");                       // one letter misread
    expectUnsaid(memory, "Мы должны уйти. Сейчас же!", 3.0, "Сейчас же!"); // the subtitle grew
    expectUnsaid(memory, "Мы должны уйти.", 30.0, "Мы должны уйти.");       // long after: said again

    // One character, one speaker: misread names are the name first seen.
    NameBook names;
    const auto name = [&](const char* n) { return encodeUtf8(names.canonical(decodeUtf8(n))); };
    const auto same = [&](const char* n, const char* want) {
        const auto got = name(n);
        if (got != want) { ++failures; std::printf("FAIL name %s -> [%s] want [%s]\n", n, got.c_str(), want); }
    };
    same("Люсия", "Люсия");
    same("Лусия", "Люсия");       // one letter misread
    same("ЛЮСИЯ", "Люсия");       // in capitals
    same("Джейсон", "Джейсон");   // someone else
    same("Незнакомый человек", "Незнакомый человек");
    same("Незнакомый человека", "Незнакомый человек"); // as read in a real game (field report)
    same("Чезнакомый человек", "Незнакомый человек");
    same("Коп", "Коп");
    same("Ли", "Ли");
    same("Ло", "Ло");             // short names one letter apart are two people
    const auto prefix = [&](const char* input, const char* speaker, const char* text) {
        auto line = cleanSubtitle(decodeUtf8(input));
        names.knownPrefix(line);
        if (encodeUtf8(line.speaker) != speaker || encodeUtf8(line.text) != text) {
            ++failures;
            std::printf("FAIL prefix %s -> [%s] [%s]\n", input, encodeUtf8(line.speaker).c_str(), encodeUtf8(line.text).c_str());
        }
    };
    prefix("Люсия; Да ну тебя.", "Люсия", "Да ну тебя.");   // a colon read as ";"
    prefix("Лусия| Пошли.", "Люсия", "Пошли.");
    prefix("Люсия, иди сюда.", "", "Люсия, иди сюда.");      // calling her, not her line
    prefix("Привет; как дела?", "", "Привет; как дела?");    // no known name
    prefix("Люсия: да ну тебя.", "Люсия", "да ну тебя.");   // a known name before a small letter
    prefix("Слушай: это важно.", "", "Слушай: это важно.");

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
