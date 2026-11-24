#pragma once
// Which of the text the neural reader found in the area is the subtitle, and which of its words
// name the speaker. A subtitle's letters are one flat colour lighter (or darker) than what is
// around them; every line of a game shares that colour, size and a few screen rows (learnt from
// pieces that read as sentences); it stands level. Within a subtitle, words in another colour at
// its start are the speaker's name. Used only on the neural
// reader's path; the Windows-OCR path keeps NameReader. Portable C++.
#include "veyra/engine/ScreenText.h"
#include "veyra/engine/TextPieces.h"
#include "SubtitleTracker.h"

#include <array>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace veyra::engine::screentext {
namespace detail { class StylePicture; } // one frame's pixels, with what was worked out from them

enum class WordKind : uint8_t {
    Body,   // a word of the line
    Marked, // in another colour: the speaker's name at the start, a word of the line elsewhere
    Scene,  // left out of a piece that is kept
};

struct SortedPiece {
    Piece piece;                 // as read, its words perhaps split ("Анна:Беги" -> "Анна:" "Беги")
    std::vector<WordKind> kinds; // one per word of piece.words
};

struct SubtitleText {
    std::vector<std::wstring> lines; // to be spoken, top to bottom
    std::wstring name;               // the speaker's name in its own colour ("" none)
    // With a game's own rules: (name, what is said) of each subtitle in the frame, top to bottom.
    // byParts: these are what to follow (SubtitleTracker::updateParts), not lines/name.
    std::vector<std::pair<std::wstring,std::wstring>> parts;
    bool byParts=false;
    bool clipped=false; // a kept line touches the area's left or right side: its ends may be cut off
};

// Rules known for one game on top of the general sorting.
enum class GameRules {
    None,
    // GTA VI: only subtitles that begin with "Name:" (and their following rows) are read; mission
    // lines, button hints, menus and the phone are not.
    Gta6,
};

// Words in small letters read with confidence (or three surely read words): a line of speech.
bool sentenceLike(const Piece& piece);
// The pieces as rows of text, top to bottom, each left to right (indices into pieces).
std::vector<std::vector<size_t>> pieceRows(const std::vector<Piece>& pieces);
// The lines to speak and the speaker's name from the sorted pieces of one frame.
SubtitleText linesAndName(const std::vector<SortedPiece>& sorted);

class SubtitleStyle {
public:
    using Rgb=std::array<float,3>;
    // russian: the subtitles are Russian, so text in Latin letters alone is no line by itself.
    // words.russian helps tell "Het!" (a Russian word misread) from "Ct" on a plate.
    explicit SubtitleStyle(bool russian=false,Dictionaries words={},GameRules rules=GameRules::None)
        :russian_(russian),words_(std::move(words)),rules_(rules){}
    // One frame: the pieces that belong to the subtitle, each word's kind. Learns as it goes.
    std::vector<SortedPiece> sort(const Crop& crop,std::vector<Piece> pieces);
    // One frame, all the way: sort, the game's rules, the clipped flag, lines and name. Per frame.
    SubtitleText read(const Crop& crop,std::vector<Piece> pieces);
    void setGameRules(GameRules rules){rules_=rules;}
    GameRules gameRules()const{return rules_;}
    void moved(); // the area moved: where lines stand is forgotten (their colour and size stay)
    void reset(); // another source or game: forget everything

    std::optional<Rgb> colour()const{return colour_;} // the lines' letters, once learnt
    bool dark()const{return dark_;}
    std::optional<float> letterHeight()const{return letter_;}
private:
    struct Sample { Rgb colour; bool dark; float letter; };
    struct Place { float row,middle,left; };
    using Picture=detail::StylePicture;
    using Kinds=std::vector<std::optional<WordKind>>;

    bool weak(const Piece& p)const;
    void learn(Picture& pic,const Piece& p);
    void settle();
    bool inPlace(const Piece& p)const;
    std::optional<std::vector<float>> knownRows()const;
    bool partOf(Picture& pic,const Piece& p,const Piece& line)const;
    std::optional<std::vector<WordKind>> kinds(Picture& pic,std::vector<Piece>& pieces,size_t index,bool beside);
    void paleName(Picture& pic,const Piece& p,const Rgb& body,Kinds& kinds)const;
    std::optional<Rgb> ownWhite(Picture& pic,const Piece& p)const;
    void splitWords(Picture& pic,Piece& p,const Rgb& body,const std::vector<uint8_t>& bodyColumns)const;

    bool russian_;
    Dictionaries words_;
    std::optional<Rgb> colour_;
    bool dark_=false;
    std::optional<float> letter_;
    std::deque<Sample> samples_;          // from lines that read as sentences (at most 24)
    std::deque<Place> places_;            // where those lines stood (at most 40)
    std::vector<float> rows_;             // rows of short pieces seen right beside such lines (at most 12)
    std::deque<std::wstring> learntFrom_; // the last lines learnt from (at most 8)
    std::vector<std::pair<size_t,Rgb>> whiteHere_; // this frame, before anything is learnt
    // Game rules (Gta6): rows that began with a name, their box heights and middles (at most 30 each).
    GameRules rules_=GameRules::None;
    std::deque<float> namedHeights_,namedMiddles_;
    std::optional<std::wstring> named(std::vector<SortedPiece*>& row,float across,float tall);
    std::vector<SortedPiece> pickNamed(std::vector<SortedPiece> sorted,std::vector<std::pair<std::wstring,std::wstring>>& parts);
};

} // namespace veyra::engine::screentext
