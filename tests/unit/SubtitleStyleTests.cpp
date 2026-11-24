// Subtitle text on the neural reader's path: telling the subtitle from text in the scene and the
// speaker's name by colour (SubtitleStyle), and releasing lines to speak (SubtitleTracker).
// Pictures are drawn by Painter: block letters over a dark, grey, textured scene.
#include "../../src/engine/SubtitleStyle.h"
#include "../../src/engine/SubtitleTracker.h"

#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace veyra::engine::screentext;

namespace {
int failures = 0;
void check(bool passed, const char* name) {
    std::printf("%s %s\n", passed ? "PASS" : "FAIL", name);
    if (!passed) ++failures;
}
std::string narrow(const std::wstring& s) { // for failure output only
    std::string o;
    for (wchar_t c : s) {
        const auto u = uint32_t(c);
        if (u < 0x80) o += char(u);
        else if (u < 0x800) { o += char(0xC0 | (u >> 6)); o += char(0x80 | (u & 63)); }
        else { o += char(0xE0 | (u >> 12)); o += char(0x80 | ((u >> 6) & 63)); o += char(0x80 | (u & 63)); }
    }
    return o;
}
void same(const std::wstring& got, const std::wstring& want, const char* name) {
    check(got == want, name);
    if (got != want) std::printf("     got \"%s\", want \"%s\"\n", narrow(got).c_str(), narrow(want).c_str());
}

struct Rgb8 { uint8_t r, g, b; };
constexpr Rgb8 kWhite{240, 240, 240}, kYellow{240, 220, 40}, kOrange{250, 160, 30}, kBlue{90, 170, 250},
    kPaleGreen{200, 235, 200}, kDark{30, 30, 30};

// Block letters: each character a 9 x lh "П" of 3 px strokes, 12 px apart; a colon two dots.
struct Painter {
    Crop crop;
    explicit Painter(uint32_t w = 900, uint32_t h = 220) {
        crop.w = w; crop.h = h; crop.rgba.resize(size_t(w) * h * 4);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t v = uint8_t(45 + ((x / 7 + y / 5) % 3) * 9);
                auto* p = &crop.rgba[(size_t(y) * w + x) * 4]; p[0] = p[1] = p[2] = v; p[3] = 255;
            }
    }
    void rect(int x0, int y0, int x1, int y1, Rgb8 c) {
        for (int y = std::max(0, y0); y < std::min(int(crop.h), y1); ++y)
            for (int x = std::max(0, x0); x < std::min(int(crop.w), x1); ++x) {
                auto* p = &crop.rgba[(size_t(y) * crop.w + x) * 4]; p[0] = c.r; p[1] = c.g; p[2] = c.b;
            }
    }
    void glyph(int x, int y, int lh, wchar_t ch, Rgb8 c) {
        if (ch == L' ') return;
        if (ch == L':') { rect(x + 3, y + 3, x + 6, y + 6, c); rect(x + 3, y + lh - 6, x + 6, y + lh - 3, c); return; }
        rect(x, y, x + 3, y + lh, c); rect(x + 6, y, x + 9, y + lh, c); rect(x, y, x + 9, y + 3, c);
    }
    // A run of text; each part has its colour; glued parts form one word. Returns the piece the
    // reader would give: its quad 4 px around the letters, the words with their left and right.
    struct Part { std::wstring text; Rgb8 colour; bool glued = false; };
    Piece line(int x, int y, const std::vector<Part>& parts, float score = 0.95f, int lh = 16, float leanDeg = 0) {
        Piece p; p.score = score;
        int cx = x;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i && !parts[i].glued) { cx += 12; p.text += L' '; }
            const int start = cx;
            for (wchar_t ch : parts[i].text) { glyph(cx, y, lh, ch, parts[i].colour); cx += 12; }
            p.text += parts[i].text;
            if (parts[i].glued && !p.words.empty()) { p.words.back().text += parts[i].text; p.words.back().right = float(cx - 3); }
            else p.words.push_back({parts[i].text, float(start), float(cx - 3)});
        }
        const float l = float(x - 4), r = float(cx + 1), t = float(y - 4), b = float(y + lh + 4);
        const float rise = std::tan(leanDeg * 3.14159265f / 180.0f) * (r - l);
        p.quad = {{{l, t}, {r, t - rise}, {r, b - rise}, {l, b}}};
        return p;
    }
};

int centreX(const std::vector<Painter::Part>& parts, int middle) {
    int width = -3;
    for (size_t i = 0; i < parts.size(); ++i) width += int(parts[i].text.size()) * 12 + (i && !parts[i].glued ? 12 : 0);
    return middle - width / 2;
}

SubtitleText readOne(SubtitleStyle& style, const Painter& p, std::vector<Piece> pieces) {
    return style.read(p.crop, std::move(pieces));
}

void sceneText() {
    {
        SubtitleStyle style(true);
        Painter p;
        p.rect(100, 20, 260, 64, {235, 235, 235}); // a light board with dark letters
        auto sign = p.line(120, 32, {{L"ВЫХОД", kDark}}, 0.97f);
        auto plate = p.line(600, 30, {{L"А123ВС", kWhite}}, 0.9f);
        auto sub = p.line(150, 160, {{L"Нам", kWhite}, {L"пора", kWhite}, {L"уходить", kWhite}, {L"отсюда.", kWhite}});
        const auto out = readOne(style, p, {sign, plate, sub});
        check(out.lines.size() == 1, "sign and number plate left out, the subtitle kept");
        if (!out.lines.empty()) same(out.lines[0], L"Нам пора уходить отсюда.", "...the subtitle read whole");
        check(out.name.empty(), "...no name");
    }
    {
        SubtitleStyle style(true);
        Painter p;
        auto leaning = p.line(500, 40, {{L"ПРОДАЮ", kWhite}}, 0.9f, 16, 10.0f);
        check(readOne(style, p, {leaning}).lines.empty(), "a leaning sign in the scene is left out");
        Painter q;
        auto level = q.line(500, 40, {{L"ПРОДАЮ", kWhite}}, 0.9f);
        check(readOne(style, q, {level}).lines.size() == 1, "...the same text level and alone is read (cannot be told)");
    }
    {
        // Once the lines' rows are known, white capitals elsewhere are a sign.
        SubtitleStyle style(true);
        const wchar_t* said[] = {L"Ты опять опоздал", L"Где тебя носило", L"Сколько можно ждать", L"Пошли быстрее",
                                 L"Машина уже ждёт", L"Не отставай"};
        for (const auto* s : said) {
            Painter p;
            std::vector<Painter::Part> parts;
            std::wstring w;
            for (const wchar_t* c = s;; ++c) {
                if (*c == L' ' || !*c) { parts.push_back({w, kWhite}); w.clear(); if (!*c) break; } else w += *c;
            }
            readOne(style, p, {p.line(200, 160, parts)});
        }
        Painter p;
        auto sign = p.line(300, 30, {{L"ЛОМБАРД", kWhite}}, 0.95f);
        auto sub = p.line(200, 160, {{L"Вот", kWhite}, {L"и", kWhite}, {L"всё", kWhite}});
        const auto out = readOne(style, p, {sign, sub});
        check(out.lines.size() == 1 && out.lines[0] == L"Вот и всё", "a white sign off the lines' rows is left out; a short line on them read");
        check(style.colour() && (*style.colour())[0] > 200 && (*style.colour())[2] > 200, "...the lines' white was learnt");
    }
}

void yellow() {
    SubtitleStyle style(true);
    Painter a;
    auto first = a.line(150, 160, {{L"Где", kYellow}, {L"ты", kYellow}, {L"был", kYellow}, {L"всё", kYellow}, {L"время?", kYellow}});
    const auto out = readOne(style, a, {first});
    check(out.lines.size() == 1 && out.lines[0] == L"Где ты был всё время?", "yellow subtitles are read");
    check(style.colour() && (*style.colour())[2] < 100, "...yellow is learnt as the lines' colour");
    Painter b;
    auto named = b.line(150, 160, {{L"Анна:", kBlue}, {L"Иди", kYellow}, {L"сюда,", kYellow}, {L"быстро!", kYellow}});
    const auto two = readOne(style, b, {named});
    same(two.name, L"Анна:", "a blue name before yellow lines");
    check(two.lines.size() == 1 && two.lines[0] == L"Иди сюда, быстро!", "...not in the spoken line");
}

void names() {
    {
        SubtitleStyle style(true);
        Painter p;
        auto line = p.line(120, 160, {{L"Джейсон:", kOrange}, {L"Где", kWhite}, {L"мои", kWhite}, {L"деньги,", kWhite}, {L"приятель?", kWhite}});
        const auto out = readOne(style, p, {line});
        same(out.name, L"Джейсон:", "a coloured name with a colon is the name");
        check(out.lines.size() == 1 && out.lines[0] == L"Где мои деньги, приятель?", "...and is not spoken");
    }
    {
        SubtitleStyle style(true);
        Painter p;
        auto line = p.line(120, 160, {{L"Сотрудник", kPaleGreen}, {L"полиции:", kPaleGreen}, {L"Стоять", kWhite}, {L"на", kWhite}, {L"месте!", kWhite}});
        const auto out = readOne(style, p, {line});
        same(out.name, L"Сотрудник полиции:", "a pale green name (within white's tolerance) is still the name");
        check(out.lines.size() == 1 && out.lines[0] == L"Стоять на месте!", "...and is not spoken");
    }
    {
        SubtitleStyle style(true);
        Painter p;
        auto line = p.line(120, 160, {{L"Анна:", kOrange}, {L"Беги", kWhite, true}, {L"отсюда", kWhite}, {L"скорее!", kWhite}});
        check(line.words.size() == 3 && line.words[0].text == L"Анна:Беги", "(the reader gives the glued word as one)");
        const auto out = readOne(style, p, {line});
        same(out.name, L"Анна:", "a name glued to the answer with no gap is cut off at the colour change");
        check(out.lines.size() == 1 && out.lines[0] == L"Беги отсюда скорее!", "...the answer is spoken");
    }
    {
        SubtitleStyle style(true);
        Painter p;
        auto line = p.line(120, 160, {{L"Найди", kWhite}, {L"Феникс", kOrange}, {L"на", kWhite}, {L"стоянке", kWhite}});
        const auto out = readOne(style, p, {line});
        check(out.name.empty() && out.lines.size() == 1 && out.lines[0] == L"Найди Феникс на стоянке",
              "a coloured word inside the line is spoken, not a name");
    }
}

void strayAndClipped() {
    {
        SubtitleStyle style(true);
        Painter p;
        auto line = p.line(120, 160, {{L"Джейсон:l", kOrange}, {L"Где", kWhite}, {L"мои", kWhite}, {L"деньги,", kWhite}, {L"приятель?", kWhite}});
        const auto out = readOne(style, p, {line});
        same(out.name, L"Джейсон:", "a stray letter after the name's colon is dropped");
        check(!out.clipped, "...a line inside the area is not clipped");
    }
    {
        SubtitleStyle style(true);
        Painter p;
        auto line = p.line(2, 160, {{L"ещё", kWhite}, {L"немного", kWhite}, {L"и", kWhite}, {L"всё", kWhite}, {L"закончится", kWhite}});
        const auto out = readOne(style, p, {line});
        check(out.lines.size() == 1 && out.clipped, "a line touching the area's side is flagged clipped");
    }
}

void gta6() {
    SubtitleStyle style(true, {}, GameRules::Gta6);
    Painter p(1400, 420);
    const int mid = 700;
    const std::vector<Painter::Part> a1{{L"Джейсон:", kOrange}, {L"Тебе", kWhite}, {L"нужно", kWhite}, {L"спрятаться,", kWhite}};
    const std::vector<Painter::Part> a2{{L"пока", kWhite}, {L"копы", kWhite}, {L"не", kWhite}, {L"уехали.", kWhite}};
    const std::vector<Painter::Part> b1{{L"Лусия:", kBlue}, {L"Я", kWhite}, {L"за", kWhite}, {L"рулём,", kWhite}, {L"садись!", kWhite}};
    const std::vector<Painter::Part> mission{{L"Ограбьте", kWhite}, {L"базу", kWhite}, {L"у", kWhite}, {L"реки.", kWhite}};
    // Rows 48 px apart (boxes 45 px), the next speaker 58 px lower, the mission line 66 px under it.
    auto r1 = p.line(centreX(a1, mid), 40, a1, 0.95f, 37);
    auto r2 = p.line(centreX(a2, mid), 88, a2, 0.95f, 37);
    auto r3 = p.line(centreX(b1, mid), 146, b1, 0.95f, 37);
    auto r4 = p.line(centreX(mission, mid), 212, mission, 0.95f, 33);
    auto hint = p.line(1100, 330, {{L"L1", kWhite}, {L"Nipper", kWhite}, {L".38", kWhite}, {L"pistol", kWhite}}, 0.9f, 30);
    const auto out = readOne(style, p, {hint, r4, r3, r2, r1});
    check(out.byParts && out.parts.size() == 2, "GTA VI: two named subtitles, nothing else");
    if (out.parts.size() == 2) {
        same(out.parts[0].first, L"Джейсон:", "...the first speaker");
        same(out.parts[0].second, L"Тебе нужно спрятаться, пока копы не уехали.", "...his two rows together");
        same(out.parts[1].first, L"Лусия:", "...the second speaker 58 px lower is her own subtitle");
        same(out.parts[1].second, L"Я за рулём, садись!", "...with what she says");
    }
    check(out.lines.size() == 3 && out.name == L"Джейсон:", "...the mission line and the button hint are not read");
    check(!out.clipped, "...nothing clipped");
    Painter q(1400, 420);
    const std::vector<Painter::Part> pale{{L"Прохожий:", kWhite}, {L"Эй,", kWhite}, {L"смотри", kWhite}, {L"куда", kWhite}, {L"идёшь!", kWhite}};
    const std::vector<Painter::Part> loose{{L"Ограбьте", kWhite}, {L"базу", kWhite}, {L"у", kWhite}, {L"реки.", kWhite}};
    auto named = q.line(centreX(pale, mid), 88, pale, 0.95f, 37);
    auto unnamed = q.line(centreX(loose, mid), 200, loose, 0.95f, 37);
    const auto two = readOne(style, q, {named, unnamed});
    check(two.parts.size() == 1 && two.parts[0].first == L"Прохожий:" && two.parts[0].second == L"Эй, смотри куда идёшь!",
          "a name whose colour cannot be told is known by 'Name:'; an unnamed row alone is not read");
    Painter g(1400, 420);
    const std::vector<Painter::Part> left{{L"Джейсон:", kOrange}, {L"Ну", kWhite}, {L"давай", kWhite}};
    auto row = g.line(400, 88, left, 0.95f, 37);
    auto menu = g.line(1250, 88, {{L"РЕЙТИНГ", kWhite}}, 0.95f, 37);
    const auto three = readOne(style, g, {row, menu});
    check(three.parts.size() == 1 && three.parts[0].second == L"Ну давай", "a word far off on the same row is not part of the subtitle");
}

void twoLines() {
    {
        SubtitleStyle style(true);
        Painter p;
        auto top = p.line(150, 130, {{L"Мы", kWhite}, {L"должны", kWhite}, {L"уйти", kWhite}, {L"отсюда,", kWhite}});
        auto bottom = p.line(150, 160, {{L"пока", kWhite}, {L"они", kWhite}, {L"не", kWhite}, {L"вернулись.", kWhite}});
        const auto out = readOne(style, p, {bottom, top});
        check(out.lines.size() == 2 && out.lines[0] == L"Мы должны уйти отсюда," && out.lines[1] == L"пока они не вернулись.",
              "a two-line subtitle comes top to bottom");
    }
    {
        SubtitleStyle style(true);
        Painter p;
        auto top = p.line(150, 130, {{L"Ширли:", kOrange}, {L"Ты", kWhite}, {L"где", kWhite}, {L"была", kWhite}, {L"вчера?", kWhite}});
        auto bottom = p.line(150, 160, {{L"Джейсон:", kBlue}, {L"Дома,", kWhite}, {L"а", kWhite}, {L"что", kWhite}, {L"такое?", kWhite}});
        const auto out = readOne(style, p, {top, bottom});
        same(out.name, L"Ширли:", "two speakers: the first one names the subtitle");
        check(out.lines.size() == 2 && out.lines[0] == L"Ты где была вчера?" && out.lines[1] == L"Дома, а что такое?",
              "...the second speaker's name is not spoken");
    }
}

void tracker() {
    {
        SubtitleTracker t;
        check(!t.update({L"Привет, как дела?"}, 0), "one reading is not enough");
        check(!t.update({L"Привет, как дела?"}, 100), "two readings within the settle time wait");
        check(t.unsettled(), "...and the tracker says text is waiting");
        const auto got = t.update({L"Привет, как дела?"}, 200);
        check(got && got->speech == L"Привет, как дела?" && got->appearedMs == 0, "released once readings agree over the settle time");
        check(!t.update({L"Привет, как дела?"}, 300), "the same subtitle still on screen is not spoken again");
        t.update({}, 400);
        t.update({L"Привет, как дела?"}, 500);
        check(!t.update({L"Привет, как дела?"}, 700), "the line shown again within memory is a repeat");
    }
    {
        Dictionaries d;
        const std::set<std::wstring> known{L"иду", L"домой"};
        d.russian = [known](std::wstring_view w) { return known.count(std::wstring(w)) > 0; };
        SubtitleTracker t({}, d);
        t.update({L"Я иду дамой."}, 0);
        t.update({L"Я иду домой."}, 100);
        const auto got = t.update({L"Я иду дамой."}, 200);
        check(got && got->speech == L"Я иду домой.", "the reading of real words is chosen over the more frequent misreading");
        SubtitleTracker plain;
        plain.update({L"Я иду дамой."}, 0);
        plain.update({L"Я иду домой."}, 100);
        const auto agreed = plain.update({L"Я иду дамой."}, 200);
        check(agreed && agreed->speech == L"Я иду дамой.", "...without a dictionary the reading most like the others");
    }
    {
        SubtitleTracker t;
        t.update({L"Слушай"}, 0);
        t.update({L"Слушай меня внимательно."}, 50);
        check(!t.update({L"Слушай"}, 200), "a fuller reading is waited for while the line may still appear");
        const auto got = t.update({L"Слушай меня внимательно."}, 250);
        check(got && got->speech == L"Слушай меня внимательно." && got->appearedMs == 0, "...and the fuller reading is released");
        t.update({L"Слушай меня внимательно. Мы уходим сейчас."}, 400);
        const auto grown = t.update({L"Слушай меня внимательно. Мы уходим сейчас."}, 600);
        check(grown && grown->speech == L"Мы уходим сейчас.", "a growing line speaks only its new part");
    }
    {
        SubtitleTracker t;
        t.update({L"Ну и что теперь?"}, 0, true, L"Tpевop:");
        const auto first = t.update({L"Ну и что теперь?"}, 200, true, L"Tpевop:");
        check(first && first->speaker == L"Тревор" && first->speech == L"Ну и что теперь?",
              "a coloured name by vote, Latin look-alikes put back");
        t.update({L"Трввор: Поехали отсюда."}, 1000);
        const auto misread = t.update({L"Трввор: Поехали отсюда."}, 1200);
        check(misread && misread->speaker == L"Тревор" && misread->speech == L"Поехали отсюда.", "a misread name maps to the known one");
        t.update({L"Тревор; Держись крепче."}, 2000);
        const auto semi = t.update({L"Тревор; Держись крепче."}, 2200);
        check(semi && semi->speaker == L"Тревор" && semi->speech == L"Держись крепче.", "a known name with a misread colon");
        t.update({L"Ты где? Тревор: Дома."}, 3000);
        const auto second = t.update({L"Ты где? Тревор: Дома."}, 3200);
        check(second && second->speech == L"Ты где? Дома.", "a coloured name's 'Name:' inside the line is removed");
        t.update({L"[смеётся] Ну ты даёшь!"}, 4000);
        const auto bracket = t.update({L"[смеётся] Ну ты даёшь!"}, 4200);
        check(bracket && bracket->speech == L"Ну ты даёшь!" && bracket->speaker.empty(), "bracketed sound descriptions are removed");
        t.update({L"Он сказал: >кди меня, Hy жe!"}, 5000);
        const auto fixed = t.update({L"Он сказал: >кди меня, Hy жe!"}, 5200);
        check(fixed && fixed->speech == L"Он сказал: жди меня, Ну же!", "Russian OCR misreads are repaired");
        if (fixed && fixed->speech != L"Он сказал: жди меня, Ну же!") std::printf("     got \"%s\"\n", narrow(fixed->speech).c_str());
        check(t.speakers().size() == 1, "...one speaker known");
        check(!t.update({L"a", L"b", L"c", L"d", L"e"}, 6000) && !t.unsettled(), "a page of text is no subtitle");
    }
    {
        using Parts = std::vector<std::pair<std::wstring, std::wstring>>;
        SubtitleTracker t;
        check(!t.updateParts(Parts{{L"Джейсон:", L"Привет, как ты?"}}, 0), "parts: one reading waits");
        const auto a = t.updateParts(Parts{{L"Джейсон:", L"Привет, как ты?"}}, 200);
        check(a && a->speaker == L"Джейсон" && a->speech == L"Привет, как ты?" && a->appearedMs == 0, "parts: released after settling");
        check(!t.updateParts(Parts{{L"Джейсон:", L"Привет, как ты?"}, {L"Лусия:", L"Нормально, а ты?"}}, 400) && t.unsettled(),
              "parts: the old subtitle stays on screen unspoken, the new one waits");
        check(t.onScreen() && t.onScreen()->speaker == L"Джейсон", "...the old one is still on screen");
        const auto b = t.updateParts(Parts{{L"Джейсон:", L"Привет, как ты?"}, {L"Лусия:", L"Нормально, а ты?"}}, 600);
        check(b && b->speaker == L"Лусия" && b->speech == L"Нормально, а ты?" && b->appearedMs == 400, "parts: the second speaker in her own voice");
        t.updateParts(Parts{{L"Джейсон:", L"Поехали отсюда."}, {L"Лусия:", L"Я за рулём."}}, 1000);
        const auto c = t.updateParts(Parts{{L"Джейсон:", L"Поехали отсюда."}, {L"Лусия:", L"Я за рулём."}}, 1200);
        const auto d = t.updateParts(Parts{{L"Джейсон:", L"Поехали отсюда."}, {L"Лусия:", L"Я за рулём."}}, 1250);
        check(c && c->speaker == L"Джейсон" && d && d->speaker == L"Лусия", "parts: two ready at once come one per call");
        check(!t.updateParts(Parts{{L"Джейсон:", L"Поехали отсюда."}, {L"Лусия:", L"Я за рулём."}}, 1400), "parts: spoken ones are not repeated");
        t.updateParts(Parts{{L"Лусия:", L"Держись крепче!"}}, 2000);
        t.updateParts(Parts{}, 3100);
        check(!t.unsettled(), "parts: a subtitle unseen for over 1 s is dropped");
        t.updateParts(Parts{{L"Лусия:", L"Держись крепче!"}}, 3200);
        check(!t.updateParts(Parts{{L"Лусия:", L"Держись крепче!"}}, 3300), "...and seen again it settles anew");
        const auto e = t.updateParts(Parts{{L"Лусия:", L"Держись крепче! Сейчас тряхнёт."}}, 3400);
        check(!e, "...a reading that grew is a different line, waiting again");
        const auto f = t.updateParts(Parts{{L"Лусия:", L"Держись крепче!"}}, 3400);
        check(f && f->speech == L"Держись крепче!", "...the settled reading released");
        t.updateParts(Parts{{L"Лусия:", L"Держись крепче! Сейчас тряхнёт."}}, 3500);
        const auto g = t.updateParts(Parts{{L"Лусия:", L"Держись крепче! Сейчас тряхнёт."}}, 3700);
        check(g && g->speech == L"Сейчас тряхнёт." && t.speakers().size() == 2, "parts: only the new part of a growing subtitle");
        Dictionaries dict;
        dict.russian = [](std::wstring_view w) { return w == L"иду" || w == L"домой"; };
        SubtitleTracker u({}, dict);
        u.updateParts(Parts{{L"Анна:", L"Я иду дамой."}}, 0);
        u.updateParts(Parts{{L"Анна:", L"Я иду домой."}}, 100);
        const auto h = u.updateParts(Parts{{L"Анна:", L"Я иду дамой."}}, 200);
        check(h && h->speech == L"Я иду домой.", "parts: the reading of real words is chosen");
    }
    {
        Dictionaries none;
        same(subtext::fixMisreads(L"OH?", none), L"Он?", "a short line read in Latin look-alikes is Russian");
        same(subtext::fixMisreads(L"Иди в 1город", none), L"Иди в город", "a speck read as a digit before a word goes");
        same(subtext::cyrillicLookalikes(L"Найди Рhоепих", none), L"Найди Phoenix", "a Latin word read half in Cyrillic");
        Dictionaries ru;
        ru.russian = [](std::wstring_view w) { return w == L"нет"; };
        same(subtext::fixMisreads(L"Het, не надо", ru), L"Нет, не надо", "a whole Latin word that is a Russian word");
        same(subtext::fixMisreads(L"HOME не здесь", ru), L"HOME не здесь", "...a Latin word that is not stays");
        check(subtext::ratio(L"abcd", L"bcde") == 0.75, "difflib-style ratio");
    }
}
} // namespace

int main() {
    sceneText();
    yellow();
    names();
    strayAndClipped();
    gta6();
    twoLines();
    tracker();
    std::printf("%s\n", failures ? "FAILURES" : "ALL PASSED");
    return failures ? 1 : 0;
}
