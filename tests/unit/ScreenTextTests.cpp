// On-screen subtitle reading (ScreenText.h): the cases VI Translator's own tests cover
// (ocr.rs), plus the pass selection and line timing with a scripted OCR engine.
#include "veyra/engine/ScreenText.h"

#include <clocale>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

using namespace veyra::engine::screentext;

namespace {
int failures = 0;
void check(bool passed, const char* name) {
    std::printf("%s %s\n", passed ? "PASS" : "FAIL", name);
    if (!passed) ++failures;
}
Word w(const wchar_t* text, uint32_t x, uint32_t y, uint32_t width, uint32_t line = 0) {
    return {text, {x, y, width, 24}, line};
}
std::wstring texts(const std::vector<Word>& words) {
    std::wstring out;
    for (const auto& word : words) { if (!out.empty()) out += L' '; out += word.text; }
    return out;
}

void contractions() {
    check(fixContractions(L"Cmon mister, were giving you everything you want.") ==
          L"C'mon mister, we're giving you everything you want.", "lost apostrophes put back");
    check(fixContractions(L"They were giving it away.") == L"They were giving it away.", "a real 'were' stays");
    check(fixContractions(L"Dont even think about it. Im serious, youre dead.") ==
          L"Don't even think about it. I'm serious, you're dead.", "several contractions in one line");
    check(fixContractions(L"Were going in!") == L"We're going in!", "line start without subject");
    check(fixContractions(L"Go to the guard.") == L"Go to the guard.", "plain line untouched");
}

void words() {
    check(!looksLikeWords(L"bjlwg"), "Arabic menu misread as Latin is not words");
    check(!looksLikeWords(L"ulnJiJl hjluJ9"), "capitals inside lowercase are not words");
    check(looksLikeWords(L"Go to the guard."), "a sentence is words");
    check(looksLikeWords(L"Hmm."), "vowel-less interjection");
    check(looksLikeWords(L"LSPD"), "acronym");
    check(looksLikeWords(L"Strengths"), "long consonant run of a real word");
    check(looksLikeWords(L"$500"), "no words at all is left as it is");
    check(clean({w(L"my.leg", 0, 0, 60), w(L"hurts", 70, 0, 50)}) == L"my leg hurts", "glued words split at the dot");
    check(clean({w(L"e.g.", 0, 0, 40), w(L"this", 50, 0, 40)}) == L"e.g. this", "abbreviation kept");
    check(clean({w(L"|", 0, 0, 4), w(L"~", 10, 0, 4)}).empty(), "noise is dropped");
    check(clean({w(L"你好", 0, 0, 40)}, false) == L"你好", "non-English text kept with two letters");
}

void similarity() {
    check(similar(L"Go to the guard.", L"Go to the guard"), "missing full stop is the same line");
    check(similar(L"GO TO THE GUARD.", L"go to the guard."), "case-insensitive");
    check(!similar(L"Go to the guard.", L"Get in the car."), "different lines");
    check(!similar(L"", L"x"), "empty is never similar");
    check(agree(L"Do not move. Idiot.", L"Do not move."), "a missed grey line still agrees");
    check(agree(L"", L""), "nothing read by both agrees");
    check(!agree(L"", L"to aim uold"), "one read nothing");
}

void centred() {
    // A 1920 frame, the area from x 192: the subtitle in the middle, a stats panel at the right.
    const std::vector<Word> panel{w(L"STRENGTH", 1291, 10, 110), w(L"Joey:", 490, 95, 58), w(L"to", 900, 95, 20),
                                  w(L"the", 925, 95, 36), w(L"action", 966, 95, 80), w(L"RATING", 1291, 70, 76)};
    check(texts(centeredWords(panel, 192, 1920)) == L"Joey: to the action", "side panel left out of the subtitle");
    const std::vector<Word> both{w(L"ADE", 3, 41, 45), w(L"STAGE", 57, 41, 74), w(L"2", 140, 41, 14),
                                 w(L"STRENGTH", 1292, 28, 90), w(L"RATING", 1292, 93, 64), w(L"350", 1481, 93, 33)};
    check(centeredWords(both, 192, 1920).empty(), "panels on both sides are not a subtitle");
    const std::vector<Word> line{w(L"Come", 700, 10, 60), w(L"on,", 768, 10, 40), w(L"let's", 816, 10, 60), w(L"go.", 884, 10, 40)};
    check(centeredWords(line, 192, 1920).size() == 4, "a centred line is kept whole");
    check(centeredWords({w(L"Browse", 1400, 10, 90)}, 192, 1920).empty(), "text only at the side is not a subtitle");
    // Veyra: a long line with its last words missed (or a game that does not centre exactly)
    // is still the subtitle; a short line that stays on one side is not.
    std::vector<Word> partial;
    for (uint32_t i = 0; i < 9; ++i) partial.push_back(w(L"word", 500 + i * 90, 10, 80));
    check(centeredWords(partial, 192, 1920).size() == 9, "a line across the middle, a little off-centre, is kept whole");
    const std::vector<Word> left{w(L"Objective:", 40, 10, 120), w(L"reach", 170, 10, 70), w(L"the", 250, 10, 40),
                                 w(L"boat", 300, 10, 60)};
    check(centeredWords(left, 192, 1920).empty(), "a line on the left side is not a subtitle");
}

void quality_and_order() {
    const auto [good, bad] = quality({w(L"Hello,", 0, 0, 50), w(L"wOrld", 60, 0, 50)});
    check(good == 5 && bad == 5, "good and bad characters counted");
    check(wordsOutOfOrder({w(L"to", 200, 0, 20), w(L"aim", 230, 0, 30), w(L"uold", 10, 0, 40)}), "right-to-left reading detected");
    check(!wordsOutOfOrder({w(L"Hold", 10, 0, 40), w(L"to", 60, 0, 20)}), "left-to-right reading");
    check(dropJunk({w(L"\"c'", 0, 0, 10), w(L"Go", 20, 0, 20)}).size() == 1, "quote fragments dropped");
    std::vector<Word> blob{w(L"Don't", 455, 172, 116), w(L"move.", 590, 172, 111), w(L"now.", 720, 172, 71), w(L"a", 1179, 80, 172)};
    blob[3].rect.h = 154; // a lamp read as a letter, 2.5 times the text's height
    for (int i = 0; i < 3; ++i) blob[size_t(i)].rect.h = 61;
    check(texts(dropTallWords(blob)) == L"Don't move. now.", "scenery taller than the text dropped");
    check(dropTallWords({w(L"Go", 0, 0, 20), w(L"A", 30, 0, 300)}).size() == 2, "two words are left as they are");
}

// A dark picture with a white outlined bar: the OCR stand-in reports what each pass is shown.
Crop picture() {
    Crop c;
    c.w = 200; c.h = 80;
    c.rgba.assign(size_t(c.w) * c.h * 4, 0);
    for (uint32_t y = 0; y < c.h; ++y)
        for (uint32_t x = 0; x < c.w; ++x) {
            uint8_t v = 40;
            if (y >= 30 && y < 50 && x >= 40 && x < 160) v = 240;
            uint8_t* p = &c.rgba[(size_t(y) * c.w + x) * 4];
            p[0] = p[1] = p[2] = v; p[3] = 255;
        }
    return c;
}

void passes() {
    const auto crop = picture();
    const auto prepared = preprocess(crop, Prep::Brightest, 2600);
    check(prepared.w == crop.w && prepared.h == crop.h && prepared.back == 1.0, "no scaling for an 80-row crop");
    check(prepared.bgra[(size_t(40) * prepared.w + 100) * 4] == 15 && prepared.bgra[0] == 215, "bright text becomes dark on light");
    const auto small = preprocess(crop, Prep::Brightest, 100);
    check(small.w <= 100 && small.h <= 100 && small.back == 2.0, "large crops halved to the engine's limit");
    Crop tiny = crop; tiny.h = 40; tiny.rgba.resize(size_t(tiny.w) * tiny.h * 4);
    check(preprocess(tiny, Prep::Brightest, 2600).h == 80, "short crops upscaled twice");

    int calls = 0;
    // Both passes agree: two calls, sure.
    Engine same = [&](const uint8_t*, uint32_t, uint32_t) {
        ++calls;
        return std::vector<RawWord>{{0, L"Get", 40, 30, 30, 20}, {0, L"in.", 80, 30, 30, 20}};
    };
    auto reading = recognize(crop, same, 2600);
    check(calls == 2 && reading.sure && texts(reading.words) == L"Get in.", "agreeing passes read once each and are sure");
    // The passes disagree: the outlined pass runs and the reading is unsure.
    calls = 0;
    Engine differ = [&](const uint8_t*, uint32_t, uint32_t) {
        ++calls;
        if (calls == 1) return std::vector<RawWord>{{0, L"Get", 40, 30, 30, 20}, {0, L"in.", 80, 30, 30, 20}};
        if (calls == 2) return std::vector<RawWord>{{0, L"6et", 40, 30, 30, 20}};
        return std::vector<RawWord>{};
    };
    reading = recognize(crop, differ, 2600);
    check(calls == 3 && !reading.sure && texts(reading.words) == L"Get in.", "disagreeing passes take a third look");
}

void tracker() {
    LineTracker t;
    check(!t.feed(L"Get in the car.", true, 0), "first sight is not shown yet");
    check(!t.feed(L"Get in the car", true, 250), "still settling");
    check(t.feed(L"Get in the car.", true, 500) && t.shown() == L"Get in the car.", "settled line shown");
    check(!t.feed(L"Get in the car.", true, 750), "same line, no change");
    check(!t.feed(L"", true, 1000) && t.shown() == L"Get in the car.", "one empty read is a blink");
    check(t.feed(L"", true, 1600) && t.shown().empty(), "gone after the clear delay");
    check(!t.feed(L"Hold on.", false, 2000) && !t.feed(L"Hold on.", false, 2400), "unsure reading waits longer");
    check(t.feed(L"Hold on.", false, 2950) && t.shown() == L"Hold on.", "unsure reading shown after three times the wait");
    check(!t.feed(L"Watch out!", true, 3200) && t.shown() == L"Hold on.", "new line keeps the old one until it settles");
    check(t.feed(L"Watch out!", true, 3500) && t.shown() == L"Watch out!", "new line replaces the old one");
    t.reset();
    check(t.shown().empty(), "reset clears");
}

// A dark picture with words drawn as letter strokes (2 px of colour, 2 px of black outline).
struct Painter {
    Crop crop;
    uint32_t x = 10;
    Painter() { crop.w = 640; crop.h = 60; crop.rgba.assign(size_t(crop.w) * crop.h * 4, 0); for (size_t i = 3; i < crop.rgba.size(); i += 4) crop.rgba[i] = 255; }
    Word word(const wchar_t* text, uint8_t r, uint8_t g, uint8_t b, uint32_t line = 0) {
        const uint32_t width = uint32_t(std::wcslen(text)) * 14, y = 18, h = 24;
        for (uint32_t yy = y + 2; yy < y + h - 2; ++yy)
            for (uint32_t xx = x; xx < x + width; ++xx)
                if ((xx / 2) % 2 == 0) { auto* p = &crop.rgba[(size_t(yy) * crop.w + xx) * 4]; p[0] = r; p[1] = g; p[2] = b; }
        Word out{text, {x, y, width, h}, line};
        x += width + 12;
        return out;
    }
};

// A fresh reader that has already seen one subtitle in `r,g,b` alone.
NameReader readerOf(uint8_t r, uint8_t g, uint8_t b) {
    NameReader reader;
    Painter p;
    std::vector<Word> ws{p.word(L"Мы", r, g, b), p.word(L"уходим", r, g, b)};
    reader.name(p.crop, ws);
    return reader;
}
std::wstring colouredName(const Crop& crop, const std::vector<Word>& words) { NameReader r; return r.name(crop, words); }

void colouredNames() {
    {
        Painter p;
        std::vector<Word> ws{p.word(L"Анна", 250, 160, 30), p.word(L"Отойди", 235, 235, 235), p.word(L"от", 235, 235, 235), p.word(L"машины!", 235, 235, 235)};
        const auto name = colouredName(p.crop, ws);
        check(name == L"Анна", "an orange name before white words is the name");
        check(markName(L"Анна Отойди от машины!", name) == L"Анна: Отойди от машины!", "the name gets its colon");
        check(markName(L"Анна: Отойди", name) == L"Анна: Отойди", "a colon already there stays one");
        check(markName(L"Незнакомый человек: эй, ты!", L"Незнакомый человек") == L"Незнакомый человек: Эй, ты!",
              "a coloured name before a line in small letters: the line gets its capital");
        check(markName(L"Аннабель пришла", L"Анна") == L"Аннабель пришла", "a longer word is not the name");
    }
    {
        Painter p;
        std::vector<Word> ws{p.word(L"Police", 150, 230, 160), p.word(L"officers:", 150, 230, 160), p.word(L"Hands", 240, 240, 240), p.word(L"up!", 240, 240, 240)};
        check(colouredName(p.crop, ws) == L"Police officers", "a name longer than its line, but ending in a colon: a name");
        check(readerOf(240, 240, 240).name(p.crop, ws) == L"Police officers", "...and the pale green name once white lines were seen");
    }
    {
        Painter p;
        std::vector<Word> ws{p.word(L"Джон", 240, 240, 240), p.word(L"Где", 240, 220, 40), p.word(L"деньги", 240, 220, 40), p.word(L"Лебовски?", 240, 220, 40)};
        check(colouredName(p.crop, ws) == L"Джон", "yellow subtitles: the white word is the name");
        check(readerOf(240, 220, 40).name(p.crop, ws) == L"Джон", "...also when yellow lines were seen");
    }
    {
        Painter p;
        std::vector<Word> ws{p.word(L"Отойди", 235, 235, 235), p.word(L"от", 235, 235, 235), p.word(L"машины!", 235, 235, 235)};
        check(colouredName(p.crop, ws).empty(), "all one colour: no name");
    }
    {
        Painter p;
        std::vector<Word> ws{p.word(L"Иди", 235, 235, 235), p.word(L"к", 235, 235, 235), p.word(L"Анне", 250, 160, 30)};
        check(colouredName(p.crop, ws).empty(), "a coloured word at the end is read, not a name");
        check(readerOf(235, 235, 235).name(p.crop, ws).empty(), "...also when white lines were seen");
    }
    {
        Painter p;
        std::vector<Word> ws{p.word(L"нажмите", 250, 160, 30), p.word(L"чтобы", 235, 235, 235), p.word(L"войти", 235, 235, 235)};
        check(colouredName(p.crop, ws).empty(), "a lower-case coloured word is not a name");
    }
    {
        // Every line starts with a name: the colour of the lines is learnt from those.
        NameReader reader;
        Painter a;
        std::vector<Word> first{a.word(L"Джейсон:", 240, 220, 40), a.word(L"До", 235, 235, 235), a.word(L"сих", 235, 235, 235),
                                a.word(L"пор", 235, 235, 235), a.word(L"не", 235, 235, 235), a.word(L"верю", 235, 235, 235)};
        check(reader.name(a.crop, first) == L"Джейсон", "a yellow name before a longer white line");
        Painter b;
        std::vector<Word> second{b.word(L"Незнакомый", 150, 220, 230), b.word(L"человек", 150, 220, 230), b.word(L"Эй", 235, 235, 235)};
        check(reader.name(b.crop, second) == L"Незнакомый человек", "...then a long name before a short line, colon or not");
    }
    {
        Painter p;
        std::vector<Word> ws{p.word(L"Анна", 250, 160, 30), p.word(L"Мария", 250, 160, 30)};
        check(colouredName(p.crop, ws).empty(), "nothing in the body colour after it: no name");
    }
}
void nameLines() {
    auto at = [](const wchar_t* text, uint32_t x, uint32_t line) { auto v = w(text, x, line * 40, 40); v.line = line; return v; };
    check(nameOnOwnLine({at(L"Анна", 100, 0), at(L"Пойдём", 80, 1), at(L"домой.", 160, 1)}) == L"Анна", "a name on its own line");
    check(nameOnOwnLine({at(L"Старый", 80, 0), at(L"моряк:", 160, 0), at(L"Уходи.", 100, 1)}) == L"Старый моряк", "with its colon");
    check(nameOnOwnLine({at(L"Пойдём", 80, 0), at(L"домой.", 160, 0)}).empty(), "one line: no name");
    check(nameOnOwnLine({at(L"Мы", 80, 0), at(L"идём.", 160, 0), at(L"Быстрее!", 100, 1)}).empty(), "a sentence above is speech");
    check(markName(L"Анна Пойдём домой.", L"Анна") == L"Анна: Пойдём домой.", "marked as a name");
}
} // namespace

int main() {
    std::setlocale(LC_ALL, "C.UTF-8"); // non-Windows builds classify non-ASCII letters through the C library
    contractions();
    words();
    similarity();
    centred();
    quality_and_order();
    passes();
    tracker();
    colouredNames();
    nameLines();
    return failures ? 1 : 0;
}
