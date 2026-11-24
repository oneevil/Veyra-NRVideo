#pragma once
// Where a game or a film shows its subtitles, found by watching the whole picture for a while
// ("find subtitles").
// Every look lists where there is text (the text finder of the neural reader, nothing read).
// Text that stays in one place look after look is the game's own (a counter, a map label);
// subtitles are lines that come and go at one height in letters of one size, wide and low,
// centred or starting from one left edge low on the screen, changing length as the words
// change. The answer is a band around them with room for a longer line and one more line.
// Portable C++.
#include <optional>
#include <vector>

namespace veyra::engine::screentext {

struct FoundBox { float left=0,top=0,right=0,bottom=0; };

struct FoundArea {
    int left=0,top=0,width=0,height=0; // in the picture's pixels
    int looks=0,of=0;                  // looks with a line in the band, and looks in all
};

// `looks`: the text boxes of each look, in the picture's pixels (`width` x `height`).
// Nothing when no band of subtitles was seen (nobody spoke, or the game shows none).
std::optional<FoundArea> locateSubtitles(const std::vector<std::vector<FoundBox>>& looks,int width,int height);

} // namespace veyra::engine::screentext
