#pragma once
// Turns the lines read off the picture frame after frame (the neural reader's path, sorted by
// SubtitleStyle) into lines to speak: a line is released once its readings agree, spoken once,
// and carries the speaker's name. The Windows-OCR path keeps LineTracker.
// Portable C++.
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace veyra::engine::screentext {

// Is this word (given in lower case) a known word? Empty: no dictionary, every reading counts
// the same and look-alike Latin words are left as they are.
using WordCheck=std::function<bool(std::wstring_view)>;
struct Dictionaries {
    WordCheck russian; // e.g. the stress dictionary's word list
    WordCheck english; // optional: picks the English reading of a word read half in Cyrillic
};

enum class NameStyle { Colon, Line, None }; // "Name: text" / name on its own first line / no names

// Text helpers shared by SubtitleStyle and SubtitleTracker.
namespace subtext {
bool isLetter(wchar_t c);
bool isLower(wchar_t c);
bool isUpper(wchar_t c);
bool isDigit(wchar_t c);
bool isAlnum(wchar_t c);
bool isSpace(wchar_t c);
bool isWordChar(wchar_t c); // a letter, a digit or '_'
bool isCyrillic(wchar_t c); // А-Я, а-я, Ё, ё
wchar_t toLower(wchar_t c);
wchar_t toUpper(wchar_t c);
std::wstring lowered(std::wstring_view s);
std::vector<std::wstring> splitWords(std::wstring_view s);
std::wstring squeeze(std::wstring_view s); // whitespace runs as one space, trimmed
// Lower case, ё as е; runs of non-word characters become one space (or nothing).
std::wstring plain(std::wstring_view s,bool spaces=true);
// How alike two strings are: twice the matched characters over both lengths (difflib's ratio).
double ratio(std::wstring_view a,std::wstring_view b);
bool similar(std::wstring_view a,std::wstring_view b,double threshold=0.88);
// Without [sound descriptions]; an unclosed bracket takes the rest of the line.
std::wstring withoutBracketed(std::wstring_view s);
// Latin look-alike letters inside Russian text put back as Cyrillic ("Het!" -> "Нет!").
std::wstring cyrillicLookalikes(std::wstring_view s,const Dictionaries& d);
// Typical OCR mistakes in Russian lines, then the look-alikes.
std::wstring fixMisreads(std::wstring_view s,const Dictionaries& d);
double realWords(std::wstring_view s,const WordCheck& russian); // share of known words, 1 without a dictionary
bool knownRussian(std::wstring_view s,const WordCheck& russian);
std::wstring cleanName(std::wstring_view s);
bool looksLikeName(std::wstring_view s,bool coloured=false);
struct Split { std::wstring speaker,speech; }; // speaker empty: none
Split splitSpeaker(const std::vector<std::wstring>& lines,NameStyle style);
} // namespace subtext

struct SpokenLine {
    std::wstring speaker; // "" when none
    std::wstring speech;
    int64_t appearedMs=0; // when the subtitle first showed up
};

struct TrackerOptions {
    int64_t settleMs=150;    // readings of a line must agree over this long (the neural reader)
    int64_t memoryMs=20000;  // how long a spoken line is remembered against repeats
    size_t maxChars=240;     // more text than this is a page, not a subtitle
    bool russian=true;       // misread repairs and the dictionary apply
    NameStyle names=NameStyle::Colon;
};

class SubtitleTracker {
public:
    using Options=TrackerOptions;
    explicit SubtitleTracker(Options o={},Dictionaries d={}):o_(o),d_(std::move(d)){}
    // One reading per frame: the lines to speak and the coloured name from SubtitleStyle
    // (linesAndName). fresh=false when the frame was not re-read. Returns a line when one is released.
    std::optional<SpokenLine> update(const std::vector<std::wstring>& lines,int64_t nowMs,bool fresh=true,
                                     const std::wstring& name={});
    // Parts mode (SubtitleText::byParts): (name, what is said) of each subtitle on screen, each
    // followed on its own - settled, chosen and spoken once, by its own speaker. At most one line
    // per call; another ready one comes at the next call.
    std::optional<SpokenLine> updateParts(const std::vector<std::pair<std::wstring,std::wstring>>& parts,int64_t nowMs);
    bool unsettled()const{return (!window_.empty()&&!released_)||!waiting_.empty();} // text waits: look again soon
    const std::optional<SpokenLine>& onScreen()const{return onScreen_;}
    const std::vector<std::wstring>& speakers()const{return speakers_;}
    void reset(); // new source or game: forget everything, names included
private:
    struct Reading { int64_t first=0,last=0; std::vector<std::wstring> lines; std::wstring text; int times=1; std::wstring name; };
    std::vector<const Reading*> agreeing(const std::wstring& text)const;
    std::vector<std::wstring> best(const std::vector<const Reading*>& group)const;
    std::wstring colouredName(const std::vector<const Reading*>& group)const;
    subtext::Split namedBefore(const std::vector<std::wstring>& lines)const;
    subtext::Split knownPrefix(const std::wstring& speech)const;
    std::wstring withoutNames(std::wstring speech)const;
    std::wstring canonical(const std::wstring& name);
    std::wstring onlyNew(const std::wstring& speech)const;
    bool misreading(const std::wstring& text)const;

    Options o_;
    Dictionaries d_;
    std::deque<Reading> window_;
    int64_t appeared_=0;
    bool released_=false;
    std::wstring screenText_;
    std::optional<SpokenLine> onScreen_;
    std::vector<std::wstring> speakers_;     // names seen so far
    std::vector<std::wstring> colouredNames_; // names seen in a colour of their own
    std::deque<std::pair<int64_t,std::wstring>> recent_; // what was spoken, and when
    struct Waiting { std::wstring name; int64_t first=0,last=0; std::vector<std::wstring> readings; };
    std::vector<Waiting> waiting_; // parts mode: subtitles on screen not spoken yet
};

} // namespace veyra::engine::screentext
