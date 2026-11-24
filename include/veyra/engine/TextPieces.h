#pragma once
// What the neural text reader finds in a crop of the picture, shared by the reader
// (NeuralOcr), the sorting of subtitle text from the scene (SubtitleStyle) and the
// search for where the subtitles are (SubtitleFinder). Portable C++.
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace veyra::engine::screentext {

// A word of a piece: its text and where it starts and ends across, in crop pixels.
struct PieceWord {
    std::wstring text;
    float left=0,right=0;
};

// A stretch of text the reader found: one line, or a part of one.
struct Piece {
    std::wstring text;
    float score=0;                          // the reader's confidence, 0..1
    std::array<std::array<float,2>,4> quad{}; // corners in crop pixels, from the top left one clockwise
    std::vector<PieceWord> words;

    struct Box { int left=0,top=0,right=0,bottom=0; };
    Box box()const{
        float x0=quad[0][0],y0=quad[0][1],x1=x0,y1=y0;
        for(const auto& p:quad){x0=std::min(x0,p[0]);y0=std::min(y0,p[1]);x1=std::max(x1,p[0]);y1=std::max(y1,p[1]);}
        return {int(std::floor(x0)),int(std::floor(y0)),int(std::ceil(x1)),int(std::ceil(y1))};
    }
    // How far the top edge leans from level, in degrees.
    float lean()const{
        const float dx=quad[1][0]-quad[0][0],dy=quad[1][1]-quad[0][1];
        return std::abs(std::atan2(dy,dx))*57.29578f;
    }
};

} // namespace veyra::engine::screentext
