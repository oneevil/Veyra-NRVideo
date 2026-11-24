// SPDX-License-Identifier: GPL-3.0-only
// Finding the subtitle band from text boxes seen over several looks (src/engine/SubtitleFinder).
#include "veyra/engine/SubtitleFinder.h"

#include <cmath>
#include <cstdio>

using namespace veyra::engine::screentext;

namespace {
int failures = 0;
void check(bool passed, const char* name) {
    std::printf("%s %s\n", passed ? "PASS" : "FAIL", name);
    if (!passed) ++failures;
}
FoundBox centredLine(float width, float top, float tall) { return {960 - width / 2, top, 960 + width / 2, top + tall}; }
}

int main() {
    const int W = 1920, H = 1080;
    {
        // 24 looks: a HUD counter that never moves, subtitles of changing length at the bottom in
        // most of them, now and then a second line above, and a sign the camera passes.
        std::vector<std::vector<FoundBox>> looks;
        for (int i = 0; i < 24; ++i) {
            std::vector<FoundBox> boxes{{1700, 60, 1860, 100}};
            if (i % 4 != 3) boxes.push_back(centredLine(500.0f + 60.0f * float(i % 7), 960, 36));
            if (i % 6 == 0) boxes.push_back(centredLine(700, 912, 36));
            if (i == 5 || i == 6) boxes.push_back({300.0f + 40.0f * float(i), 400, 520.0f + 40.0f * float(i), 440});
            looks.push_back(boxes);
        }
        const auto found = locateSubtitles(looks, W, H);
        check(found.has_value(), "subtitles found");
        if (found) {
            std::printf("  area %d,%d %dx%d looks %d/%d\n", found->left, found->top, found->width, found->height, found->looks, found->of);
            check(found->top < 912 && found->top + found->height > 996, "the band holds both lines");
            check(found->left < 960 - 400 && found->left + found->width > 960 + 400, "centred and wide");
            check(found->top > 700, "low on the screen, not the sign");
        }
    }
    {
        // A line of large letters cut into words is one line again.
        std::vector<std::vector<FoundBox>> looks;
        for (int i = 0; i < 12; ++i) {
            const float half = 200.0f + 30.0f * float(i % 5);
            looks.push_back({{960 - half, 950, 940, 1000}, {960, 950, 960 + half, 1000}});
        }
        const auto found = locateSubtitles(looks, W, H);
        check(found.has_value() && found->left < 700, "words joined into lines");
    }
    {
        // No speech: only the HUD and a hint that stay put.
        std::vector<std::vector<FoundBox>> looks(20, {{1700, 60, 1860, 100}, {40, 40, 600, 80}});
        check(!locateSubtitles(looks, W, H), "nothing when only the game's own text is there");
    }
    {
        // Graffiti the camera passes: lines at changing heights and sizes.
        std::vector<std::vector<FoundBox>> looks;
        for (int i = 0; i < 20; ++i) {
            const float y = 200.0f + 37.0f * float(i), tall = 20.0f + 3.0f * float(i % 6);
            looks.push_back({{600.0f + 10.0f * float(i), y, 1300.0f + 10.0f * float(i), y + tall}});
        }
        check(!locateSubtitles(looks, W, H), "nothing for text that wanders");
    }
    {
        // Lines from one left edge, low on the screen.
        std::vector<std::vector<FoundBox>> looks;
        for (int i = 0; i < 16; ++i) looks.push_back({{120, 940, 120.0f + 400.0f + 50.0f * float(i % 6), 975}});
        const auto found = locateSubtitles(looks, W, H);
        check(found.has_value() && found->left < 120 && found->left + found->width > 1000, "left-aligned lines grow to the right");
    }
    {
        // A film in a player's window off the picture's middle (centre 760), with a hint at the
        // same height far to the right: the band stands on the film's own middle.
        std::vector<std::vector<FoundBox>> looks;
        for (int i = 0; i < 16; ++i) {
            const float half = 120.0f + 40.0f * float(i % 5);
            std::vector<FoundBox> boxes{{760 - half, 900, 760 + half, 930}};
            if (i % 2) boxes.push_back({1500, 902, 1800, 928});
            looks.push_back(boxes);
        }
        const auto found = locateSubtitles(looks, W, H);
        check(found.has_value(), "a film off the middle found");
        if (found) {
            const float mid = float(found->left) + float(found->width) / 2;
            check(std::abs(mid - 760) < 30 && found->left + found->width < 1500, "on the film's middle, without the hint");
            check(found->width >= int(2 * 13 * 30) - 2, "room for lines longer than those seen");
        }
    }
    check(!locateSubtitles({{}, {}, {}}, W, H), "too few looks");
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
