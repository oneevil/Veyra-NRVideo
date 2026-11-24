// SPDX-License-Identifier: GPL-3.0-only
// The narrators (src/tts/Narrators): the catalogue, how a speaker is given one, the pitch of a
// voice and the tape-like speed change that makes a narrator's variants.
#include "../../src/tts/Cast.h"
#include "../../src/tts/Narrators.h"

#include <cmath>
#include <cstdio>
#include <numbers>

using namespace veyra::tts;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}
// A voiced sound at `hz`: a few harmonics, like a vowel.
std::vector<float> vowel(float hz, int rate, float seconds) {
    std::vector<float> w(size_t(rate * seconds));
    for (size_t i = 0; i < w.size(); ++i) {
        const double t = double(i) / rate;
        w[i] = float(0.5 * std::sin(2 * std::numbers::pi * hz * t) + 0.3 * std::sin(4 * std::numbers::pi * hz * t)
                     + 0.15 * std::sin(6 * std::numbers::pi * hz * t));
    }
    return w;
}
}

int main() {
    int men = 0, women = 0, byHand = 0;
    for (const auto& n : narrators()) (n.byHandOnly ? byHand : n.female ? women : men)++;
    check(men == 9 && women == 12 && byHand == 1, "9 men, 12 women and one by hand only");
    check(findNarrator("eng_m4") && findNarrator("eng_m4")->name == "Виктор", "eng_m4 is Виктор");
    check(findNarrator("ru_f2_high") && findNarrator("ru_f2_high")->name == "Лиза", "ru_f2_high is Лиза");
    check(defaultNarrator(false).id == "eng_m4" && defaultNarrator(true).id == "eng_f5", "Виктор and Диана by default");

    check(pickNarrator(false, {}) == "eng_m4", "first man: Виктор");
    check(pickNarrator(false, {"eng_m4"}) == "ru_m5", "second man: Артём");
    check(pickNarrator(false, {"eng_m4", "ru_m5", "ru_m1"}) == "eng_m4_low", "the voices used up: a variant");
    check(pickNarrator(false, {}, 150.0f) == "ru_m1", "a man at 150 Hz: Максим (145 Hz)");
    check(pickNarrator(false, {"ru_m1"}, 150.0f) != "ru_m1_high" && pickNarrator(false, {"ru_m1"}, 150.0f) != "ru_m1_low",
          "not a variant of a narrator already speaking when another is near");
    check(pickNarrator(true, {}, 283.0f) == "ru_f1", "a woman at 283 Hz: Алиса (286 Hz)");
    check(pickNarrator(true, {}, 290.0f) == "eng_f5_high", "at 290 Hz the nearest is Мила (288 Hz), a variant");
    std::vector<std::string> allWomen;
    for (const auto& n : narrators()) if (n.female) allWomen.push_back(n.id);
    check(!pickNarrator(true, allWomen).empty(), "more speakers than narrators: shared");
    check(pickNarrator(false, {"eng_m3"}) == "eng_m4" && pickNarrator(false, {}) != "eng_m3", "Саша is never handed out");

    const auto p = medianPitch(vowel(150.0f, 16000, 1.0f), 16000);
    check(p && std::abs(*p - 150.0f) < 5.0f, "pitch of a 150 Hz vowel");
    const auto q = medianPitch(vowel(260.0f, 16000, 1.0f), 16000);
    check(q && std::abs(*q - 260.0f) < 8.0f, "pitch of a 260 Hz vowel");
    check(!medianPitch(std::vector<float>(16000, 0.0f), 16000), "silence has no pitch");

    const auto faster = playFaster(vowel(200.0f, 16000, 1.0f), 1.08f);
    const auto r = medianPitch(faster, 16000);
    check(std::abs(int(faster.size()) - int(16000 / 1.08)) <= 1, "1.08x faster is 1.08x shorter");
    check(r && std::abs(*r - 216.0f) < 6.0f, "and 1.08x higher");
    const auto slower = playFaster(vowel(200.0f, 16000, 1.0f), 0.92f);
    const auto s = medianPitch(slower, 16000);
    check(s && std::abs(*s - 184.0f) < 6.0f, "0.92x: lower");

    {
        Cast cast;
        auto& all = cast.member("", "Все реплики", {}, {});
        check(cast.narrator(all) == "eng_m4", "every line: Виктор by default");
        cast.member("@male", "", {}, {});
        cast.member("@female", "", {}, {});
        check(cast.narrator(*cast.find("@female")) == "eng_f5", "unnamed women: Диана");
        bool created = false;
        auto& anna = cast.member("name:анна", "Анна", true, {}, &created);
        check(created && cast.narrator(anna) == "ru_f1", "Анна: a woman nobody reads with (Алиса)");
        auto& tom = cast.member("name:том", "Том", false, 150.0f);
        check(cast.narrator(tom) == "ru_m1", "Том at 150 Hz: Максим");
        cast.member("name:том", "Том", false, {}, &created);
        check(!created && cast.narrator(*cast.find("name:том")) == "ru_m1", "the same character keeps his narrator");
        cast.setChoice("name:том", "eng_m3");
        check(cast.narrator(*cast.find("name:том")) == "eng_m3", "chosen by hand: Саша");
        cast.setChoice("name:том", "mute");
        check(cast.narrator(*cast.find("name:том")).empty(), "muted: nothing");
        cast.setChoice("name:том", "nonsense");
        check(cast.narrator(*cast.find("name:том")) == "ru_m1", "an unknown choice is auto");
        auto& kim = cast.member("name:ким", "Ким", {}, {});
        check(!findNarrator(cast.narrator(kim))->female, "sex unknown: a man's narrator");
        cast.member("name:ким", "Ким", true, {});
        check(!findNarrator(cast.narrator(*cast.find("name:ким")))->female, "a later guess does not change the narrator");
        check(cast.needsHearing("name:ким") && !cast.needsHearing("@male"), "not heard yet: worth listening to");
        cast.hear("name:ким", true, 270.0f);
        const auto kimVoice = cast.narrator(*cast.find("name:ким"));
        check(findNarrator(kimVoice)->female && !cast.needsHearing("name:ким"), "heard to be a woman: a woman's narrator");
        cast.hear("name:ким", false, 120.0f);
        check(cast.narrator(*cast.find("name:ким")) == kimVoice, "decided once: a later opening does not change it");
        cast.confirm("name:ким", true, {});
        cast.confirm("name:ким", false, {});
        check(cast.narrator(*cast.find("name:ким")) == kimVoice, "one whole line of the other sex is not enough");
        cast.confirm("name:ким", false, {});
        check(!findNarrator(cast.narrator(*cast.find("name:ким")))->female, "the lines say a man: a man's narrator");
        // Nothing audible twice: the guess stays.
        auto& lu = cast.member("name:люсия", "Люсия", true, {});
        const auto luVoice = cast.narrator(lu);
        cast.hear("name:люсия", std::nullopt, {});
        check(cast.needsHearing("name:люсия"), "once nothing heard: still listening");
        cast.hear("name:люсия", std::nullopt, {});
        check(!cast.needsHearing("name:люсия") && cast.narrator(*cast.find("name:люсия")) == luVoice, "twice: the guess stays");
        cast.removeIf([](const CastMember& m) { return m.key.rfind("name:", 0) == 0; });
        check(!cast.find("name:анна") && cast.find("@male"), "a new session forgets the characters only");
    }

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
