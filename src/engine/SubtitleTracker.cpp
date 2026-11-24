// Subtitle tracker for the neural reader's path (SubtitleTracker.h).
#include "SubtitleTracker.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>

namespace veyra::engine::screentext {
namespace subtext {

// --- characters (own tables: the C library knows no Cyrillic in the "C" locale) ---------
bool isLetter(wchar_t c){
    const auto u=uint32_t(c);
    if(u<128)return (c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z');
    if(u>=0xC0&&u<=0x24F)return u!=0xD7&&u!=0xF7;
    return (u>=0x370&&u<=0x3FF&&u!=0x37E&&u!=0x387)||(u>=0x400&&u<=0x52F&&!(u>=0x482&&u<=0x489))
        ||(u>=0x3040&&u<=0x30FF)||(u>=0x3400&&u<=0x9FFF)||(u>=0xAC00&&u<=0xD7AF);
}
bool isUpper(wchar_t c){
    const auto u=uint32_t(c);
    if(u<128)return c>=L'A'&&c<=L'Z';
    if(u>=0xC0&&u<=0xDE)return u!=0xD7;
    if(u>=0x100&&u<=0x17F)return u%2==0;
    if(u>=0x391&&u<=0x3AB)return true;
    if(u>=0x400&&u<=0x42F)return true;
    if(u>=0x460&&u<=0x52F)return u%2==0&&!(u>=0x482&&u<=0x489);
    return false;
}
bool isLower(wchar_t c){
    const auto u=uint32_t(c);
    if(u<128)return c>=L'a'&&c<=L'z';
    if(u>=0xDF&&u<=0xFF)return u!=0xF7;
    if(u>=0x100&&u<=0x17F)return u%2==1;
    if(u>=0x3AC&&u<=0x3CE)return true;
    if(u>=0x430&&u<=0x45F)return true;
    if(u>=0x460&&u<=0x52F)return u%2==1&&!(u>=0x482&&u<=0x489);
    return false;
}
wchar_t toLower(wchar_t c){
    const auto u=uint32_t(c);
    if(c>=L'A'&&c<=L'Z')return wchar_t(u+32);
    if(u>=0xC0&&u<=0xDE&&u!=0xD7)return wchar_t(u+32);
    if(u>=0x410&&u<=0x42F)return wchar_t(u+32);
    if(u>=0x400&&u<=0x40F)return wchar_t(u+80);
    if(u>=0x391&&u<=0x3AB&&u!=0x3A2)return wchar_t(u+32);
    if(((u>=0x100&&u<=0x17F)||(u>=0x460&&u<=0x52F&&!(u>=0x482&&u<=0x489)))&&u%2==0)return wchar_t(u+1);
    return c;
}
wchar_t toUpper(wchar_t c){
    const auto u=uint32_t(c);
    if(c>=L'a'&&c<=L'z')return wchar_t(u-32);
    if(u>=0xE0&&u<=0xFE&&u!=0xF7)return wchar_t(u-32);
    if(u>=0x430&&u<=0x44F)return wchar_t(u-32);
    if(u>=0x450&&u<=0x45F)return wchar_t(u-80);
    if(u>=0x3B1&&u<=0x3CB&&u!=0x3C2)return wchar_t(u-32);
    if(((u>=0x100&&u<=0x17F)||(u>=0x460&&u<=0x52F&&!(u>=0x482&&u<=0x489)))&&u%2==1)return wchar_t(u-1);
    return c;
}
bool isDigit(wchar_t c){return c>=L'0'&&c<=L'9';}
bool isAlnum(wchar_t c){return isLetter(c)||isDigit(c);}
bool isSpace(wchar_t c){
    const auto u=uint32_t(c);
    return c==L' '||(u>=9&&u<=13)||u==0x1C||u==0x1D||u==0x1E||u==0x1F||u==0x85||u==0xA0||u==0x1680
        ||(u>=0x2000&&u<=0x200A)||u==0x2028||u==0x2029||u==0x202F||u==0x205F||u==0x3000;
}
bool isWordChar(wchar_t c){return isAlnum(c)||c==L'_';}
bool isCyrillic(wchar_t c){return (c>=L'А'&&c<=L'я')||c==L'Ё'||c==L'ё';}

std::wstring lowered(std::wstring_view s){std::wstring o(s);for(auto& c:o)c=toLower(c);return o;}
std::vector<std::wstring> splitWords(std::wstring_view s){
    std::vector<std::wstring> out;std::wstring cur;
    for(wchar_t c:s){if(isSpace(c)){if(!cur.empty())out.push_back(std::move(cur));cur.clear();}else cur+=c;}
    if(!cur.empty())out.push_back(std::move(cur));
    return out;
}
std::wstring squeeze(std::wstring_view s){
    std::wstring o;
    for(const auto& w:splitWords(s)){if(!o.empty())o+=L' ';o+=w;}
    return o;
}
std::wstring plain(std::wstring_view s,bool spaces){
    std::wstring o;bool gap=false;
    for(wchar_t c:s){
        c=toLower(c);if(c==L'ё')c=L'е';
        if(isWordChar(c)){if(gap&&spaces&&!o.empty())o+=L' ';gap=false;o+=c;}
        else gap=true;
    }
    return o;
}

// Ratcliff-Obershelp matching as difflib does it (no junk): the longest common block, then the
// same on both sides of it.
double ratio(std::wstring_view a,std::wstring_view b){
    const size_t total=a.size()+b.size();
    if(!total)return 1.0;
    size_t matched=0;
    std::vector<size_t> len(b.size()+1),next(b.size()+1);
    struct Span { size_t alo,ahi,blo,bhi; };
    std::vector<Span> todo{{0,a.size(),0,b.size()}};
    while(!todo.empty()){
        const Span s=todo.back();todo.pop_back();
        size_t bi=s.alo,bj=s.blo,best=0;
        std::fill(len.begin(),len.end(),0);
        for(size_t i=s.alo;i<s.ahi;++i){
            std::fill(next.begin(),next.end(),0);
            for(size_t j=s.blo;j<s.bhi;++j){
                if(a[i]!=b[j])continue;
                const size_t k=(j>s.blo?len[j-1]:0)+1;
                next[j]=k;
                if(k>best){best=k;bi=i+1-k;bj=j+1-k;}
            }
            std::swap(len,next);
        }
        if(!best)continue;
        matched+=best;
        if(s.alo<bi&&s.blo<bj)todo.push_back({s.alo,bi,s.blo,bj});
        if(bi+best<s.ahi&&bj+best<s.bhi)todo.push_back({bi+best,s.ahi,bj+best,s.bhi});
    }
    return 2.0*double(matched)/double(total);
}
bool similar(std::wstring_view a0,std::wstring_view b0,double threshold){
    const auto a=plain(a0),b=plain(b0);
    if(a.empty()||b.empty())return a==b;
    return a==b||ratio(a,b)>=threshold;
}

std::wstring withoutBracketed(std::wstring_view s){
    auto opener=[](wchar_t c){return c==L'['||c==L'【';};
    auto closer=[](wchar_t c){return c==L']'||c==L'】';};
    std::wstring o(s);
    // Innermost [..] groups first, then an opener left without its closer cuts the rest.
    for(size_t i=0;i<o.size();){
        if(!opener(o[i])){++i;continue;}
        size_t j=i+1;
        while(j<o.size()&&!opener(o[j])&&!closer(o[j]))++j;
        if(j<o.size()&&closer(o[j])){o.replace(i,j-i+1,L" ");i+=1;}
        else i=j;
    }
    const auto open=std::find_if(o.begin(),o.end(),opener);
    o.erase(open,o.end());
    return squeeze(o);
}

namespace {
constexpr std::wstring_view kLookalike=L"aceopxyABCEHKMOPTX",kLookalikeRu=L"асеорхуАВСЕНКМОРТХ";
constexpr std::wstring_view kShort=L"tkmunhbr",kShortRu=L"ткмипньг";
bool in(std::wstring_view set,wchar_t c){return set.find(c)!=std::wstring_view::npos;}
std::wstring mapped(std::wstring s,std::wstring_view from,std::wstring_view to){
    for(auto& c:s){const auto at=from.find(c);if(at!=std::wstring_view::npos)c=to[at];}
    return s;
}
std::wstring asRussian(const std::wstring& s){return mapped(mapped(s,kShort,kShortRu),kLookalike,kLookalikeRu);}
bool ascii(wchar_t c){return uint32_t(c)<128;}
bool cyrLower(wchar_t c){return (c>=L'а'&&c<=L'я')||c==L'ё';}

// The Latin letters a Cyrillic look-alike may stand for, the likeliest first.
std::wstring_view asLatin(wchar_t c){
    switch(c){
    case L'а':return L"a";case L'в':return L"b";case L'г':return L"r";case L'е':case L'ё':return L"e";
    case L'з':return L"3";case L'и':return L"iu";case L'й':return L"u";case L'к':return L"k";case L'м':return L"m";
    case L'н':return L"h";case L'о':return L"o";case L'п':return L"n";case L'р':return L"p";case L'с':return L"c";
    case L'т':return L"t";case L'у':return L"y";case L'х':return L"x";case L'ш':return L"w";case L'ь':return L"b";
    case L'і':case L'ї':return L"i";case L'є':return L"e";case L'ј':return L"j";case L'ѕ':return L"s";
    case L'А':return L"A";case L'В':return L"B";case L'Е':return L"E";case L'И':return L"NU";case L'К':return L"K";
    case L'М':return L"M";case L'Н':return L"H";case L'О':return L"O";case L'П':return L"N";case L'Р':return L"P";
    case L'С':return L"C";case L'Т':return L"T";case L'У':return L"Y";case L'Х':return L"X";case L'Ш':return L"W";
    case L'Я':return L"R";case L'І':return L"I";case L'Ј':return L"J";case L'Ѕ':return L"S";
    default:return {};
    }
}
// A word mixing both alphabets with a letter that is Latin beyond doubt: the word in Latin.
std::wstring latinWord(const std::wstring& word,const WordCheck& english){
    std::vector<std::wstring> readings{L""};
    for(wchar_t c:word){
        std::wstring_view options;wchar_t self[1]={c};
        if(ascii(c))options=std::wstring_view(self,1);else options=asLatin(c);
        if(options.empty())return word;
        std::vector<std::wstring> grown;
        for(const auto& r:readings)for(wchar_t o:options){if(grown.size()<16)grown.push_back(r+o);}
        readings=std::move(grown);
    }
    if(readings.size()>1&&english)
        for(const auto& r:readings)if(english(lowered(r)))return r;
    return readings.front();
}
bool tokenChar(wchar_t c){
    return (c<128&&isLetter(c))||isCyrillic(c)||in(L"іїєјѕІЈЅ",c);
}
} // namespace

bool knownRussian(std::wstring_view s,const WordCheck& russian){
    if(!russian)return true;
    // A drawn-out word counts as spelled: "не-е-ет", "нееет" -> "нет".
    std::wstring t=lowered(s),a;
    for(size_t i=0;i<t.size();){
        a+=t[i];
        if(cyrLower(t[i])){
            size_t j=i+1;
            while(j+1<t.size()&&t[j]==L'-'&&t[j+1]==t[i])j+=2;
            i=j;
        }else ++i;
    }
    std::wstring b;
    for(size_t i=0;i<a.size();){
        size_t j=i+1;
        while(j<a.size()&&a[j]==a[i])++j;
        b.append(cyrLower(a[i])&&j-i>=3?1:j-i,a[i]);
        i=j;
    }
    std::wstring word;
    auto ok=[&]{bool good=word.size()<2||russian(word);word.clear();return good;};
    for(wchar_t c:b){if(cyrLower(c))word+=c;else if(!word.empty()&&!ok())return false;}
    return word.empty()||ok();
}

double realWords(std::wstring_view s,const WordCheck& russian){
    if(!russian)return 1.0;
    std::set<std::wstring> words;std::wstring word;
    for(wchar_t c:lowered(s)+L' '){
        if(cyrLower(c)){word+=c;continue;}
        if(word.size()>=3)words.insert(word);
        word.clear();
    }
    if(words.empty())return 1.0;
    size_t known=0;for(const auto& w:words)known+=russian(w);
    return double(known)/double(words.size());
}

std::wstring cyrillicLookalikes(std::wstring_view s0,const Dictionaries& d){
    const std::wstring s(s0);
    std::vector<wchar_t> letters;for(wchar_t c:s)if(isLetter(c))letters.push_back(c);
    if(!letters.empty()&&std::all_of(letters.begin(),letters.end(),[](wchar_t c){return in(kLookalike,c)||in(kShort,c);})){
        // A short line read wholly in Latin look-alikes ("OH?"): Russian only if it makes Russian words.
        auto fixed=asRussian(s);
        if(!knownRussian(fixed,d.russian))return s;
        bool cased=false,upper=true;
        for(wchar_t c:fixed){if(isUpper(c))cased=true;else if(isLower(c)){cased=true;upper=false;}}
        if(cased&&upper&&splitWords(fixed).size()==1&&!fixed.empty()){
            fixed[0]=toUpper(fixed[0]);
            for(size_t i=1;i<fixed.size();++i)fixed[i]=toLower(fixed[i]);
        }
        return fixed;
    }
    if(std::none_of(letters.begin(),letters.end(),[](wchar_t c){return cyrLower(toLower(c));}))return s;
    std::wstring out;
    for(size_t i=0;i<s.size();){
        if(!tokenChar(s[i])){out+=s[i++];continue;}
        size_t j=i;while(j<s.size()&&tokenChar(s[j]))++j;
        const std::wstring word=s.substr(i,j-i);i=j;
        size_t latin=0;bool allLook=true,allLookShort=true;
        for(wchar_t c:word){
            if(ascii(c)){++latin;allLook&=in(kLookalike,c);}
            allLookShort&=in(kLookalike,c)||in(kShort,c);
        }
        if(!latin){out+=word;continue;}
        if(latin<word.size()){out+=allLook?mapped(word,kLookalike,kLookalikeRu):latinWord(word,d.english);continue;}
        if(word.size()<=2){out+=allLook?mapped(word,kLookalike,kLookalikeRu):word;continue;}
        if(allLookShort&&d.russian){
            const auto fixed=asRussian(word);
            if(d.russian(lowered(fixed))){out+=fixed;continue;}
        }
        out+=word;
    }
    return out;
}

std::wstring fixMisreads(std::wstring_view s0,const Dictionaries& d){
    std::wstring s(s0),o;
    auto at=[&](const std::wstring& t,long i)->wchar_t{return i>=0&&size_t(i)<t.size()?t[size_t(i)]:L'\0';};
    // A symbol plus "к" for "ж".
    for(size_t i=0;i<s.size();++i){
        if(in(L"»>})",s[i])&&(at(s,long(i)+1)==L'к'||at(s,long(i)+1)==L'К')){o+=s[i+1]==L'к'?L'ж':L'Ж';++i;}
        else o+=s[i];
    }
    s.swap(o);o.clear();
    // Specks between letters of a word.
    for(size_t i=0;i<s.size();){
        if(in(L"*^~`|",s[i])&&i>0&&isWordChar(s[i-1])){
            size_t j=i;while(j<s.size()&&in(L"*^~`|",s[j]))++j;
            if(j<s.size()&&isWordChar(s[j])){o+=L' ';i=j;continue;}
        }
        o+=s[i++];
    }
    s.swap(o);o.clear();
    // A hyphen left hanging after a word.
    for(size_t i=0;i<s.size();++i){
        if(s[i]==L'-'&&i>0&&cyrLower(s[i-1])&&i+1<s.size()&&isSpace(s[i+1]))continue;
        o+=s[i];
    }
    s.swap(o);o.clear();
    // A digit stuck to the front of a word: "1город".
    for(size_t i=0;i<s.size();++i){
        if(isDigit(s[i])){
            const wchar_t p=at(s,long(i)-1);
            const bool free=!(p&&(isWordChar(p)||p==L','||p==L'.'));
            if(free&&cyrLower(at(s,long(i)+1))&&cyrLower(at(s,long(i)+2))&&cyrLower(at(s,long(i)+3)))continue;
        }
        o+=s[i];
    }
    s.swap(o);o.clear();
    // A lone bracket or bar between spaces.
    for(size_t i=0;i<s.size();++i){
        const wchar_t p=at(s,long(i)-1),n=at(s,long(i)+1);
        if(in(L"[]{}|<>",s[i])&&(!p||isSpace(p))&&(!n||isSpace(n))){o+=L' ';continue;}
        o+=s[i];
    }
    s.swap(o);o.clear();
    // One stray consonant with a comma: scenery read as text.
    for(size_t i=0;i<s.size();){
        const wchar_t p=at(s,long(i)-1),c=toLower(s[i]);
        const bool consonant=(c>=L'б'&&c<=L'д')||c==L'ж'||c==L'з'||(c>=L'й'&&c<=L'н')||(c>=L'п'&&c<=L'т')||(c>=L'ф'&&c<=L'щ');
        if(consonant&&(!p||isSpace(p))&&i+1<s.size()&&isSpace(s[i+1])){
            size_t j=i+1;while(j<s.size()&&isSpace(s[j]))++j;
            if(j<s.size()&&s[j]==L','){++j;while(j<s.size()&&isSpace(s[j]))++j;i=j;continue;}
        }
        o+=s[i++];
    }
    s.swap(o);o.clear();
    // The next word glued to a punctuation mark: "видела?У нас".
    for(size_t i=0;i<s.size();++i){
        o+=s[i];
        if(in(L",.!?;:",s[i])&&i>0&&isCyrillic(s[i-1])&&isCyrillic(at(s,long(i)+1)))o+=L' ';
    }
    return squeeze(cyrillicLookalikes(o,d));
}

std::wstring cleanName(std::wstring_view s){
    std::wstring o;
    for(wchar_t c:s)o+=(isWordChar(c)||in(L".'’-",c))?c:L' ';
    return squeeze(o);
}

bool looksLikeName(std::wstring_view s,bool coloured){
    const auto words=splitWords(s);
    if(words.empty()||words.size()>(coloured?4u:3u))return false;
    if(std::any_of(s.begin(),s.end(),[](wchar_t c){return in(L".!?…,;",c);}))return false;
    const size_t checked=coloured?1:words.size();
    for(size_t i=0;i<checked;++i)if(isLower(words[i][0]))return false;
    return std::any_of(s.begin(),s.end(),isLetter);
}

namespace {
// "Name: text" (the colon must be followed by a space) or "[Name] text".
bool nameColon(const std::wstring& t,std::wstring& name,std::wstring& rest){
    size_t i=0;while(i<t.size()&&isSpace(t[i]))++i;
    const size_t colon=t.find_first_of(L":：\n",i);
    if(colon==std::wstring::npos||t[colon]==L'\n')return false;
    size_t e=colon;while(e>i&&isSpace(t[e-1]))--e;
    if(e<=i||e-i>30)return false;
    size_t r=colon+1;
    if(r>=t.size()||!isSpace(t[r]))return false;
    while(r<t.size()&&isSpace(t[r]))++r;
    if(r>=t.size())return false;
    name=t.substr(i,e-i);rest=squeeze(t.substr(r));
    return true;
}
bool nameBracket(const std::wstring& t,std::wstring& name,std::wstring& rest){
    size_t i=0;while(i<t.size()&&isSpace(t[i]))++i;
    if(i>=t.size()||!in(L"[(<",t[i]))return false;
    ++i;while(i<t.size()&&isSpace(t[i]))++i;
    const size_t close=t.find_first_of(L"])>\n",i);
    if(close==std::wstring::npos||t[close]==L'\n')return false;
    size_t e=close;while(e>i&&isSpace(t[e-1]))--e;
    if(e<=i||e-i>30)return false;
    size_t r=close+1;while(r<t.size()&&isSpace(t[r]))++r;
    if(r>=t.size())return false;
    name=t.substr(i,e-i);rest=squeeze(t.substr(r));
    return true;
}
std::wstring joined(const std::vector<std::wstring>& lines,size_t from=0){
    std::wstring o;
    for(size_t i=from;i<lines.size();++i){const auto l=squeeze(lines[i]);if(l.empty())continue;if(!o.empty())o+=L' ';o+=l;}
    return o;
}
} // namespace

Split splitSpeaker(const std::vector<std::wstring>& lines,NameStyle style){
    const auto text=joined(lines);
    if(style==NameStyle::None||text.empty())return {{},text};
    if(style==NameStyle::Line){
        if(lines.size()<2)return {{},text};
        auto first=squeeze(lines[0]);
        while(!first.empty()&&first.back()==L':')first.pop_back();
        const auto name=cleanName(first);
        if(!name.empty()&&looksLikeName(name))return {name,joined(lines,1)};
        return {{},text};
    }
    std::wstring name,rest;
    if(nameColon(text,name,rest)&&looksLikeName(cleanName(name)))return {cleanName(name),rest};
    if(nameBracket(text,name,rest)&&looksLikeName(cleanName(name)))return {cleanName(name),rest};
    return {{},text};
}
} // namespace subtext

// --- the tracker -----------------------------------------------------------------------------
using namespace subtext;

namespace {
bool sameLine(const std::wstring& a,const std::wstring& b){
    const long la=long(plain(a).size()),lb=long(plain(b).size());
    return similar(a,b,0.85)&&std::abs(la-lb)<=2;
}
// "Tpевop" with Cyrillic in it is "Тревор": Latin look-alikes put back.
std::wstring nameLetters(const std::wstring& name){
    if(std::none_of(name.begin(),name.end(),isCyrillic))return name;
    for(wchar_t c:name)if(c<128&&isLetter(c)&&!in(kLookalike,c))return name;
    return mapped(name,kLookalike,kLookalikeRu);
}
std::wstring trimmed(const std::wstring& s){
    size_t a=0,b=s.size();
    while(a<b&&isSpace(s[a]))++a;
    while(b>a&&isSpace(s[b-1]))--b;
    return s.substr(a,b-a);
}
} // namespace

std::optional<SpokenLine> SubtitleTracker::update(const std::vector<std::wstring>& lines,int64_t now,bool fresh,
                                                  const std::wstring& name){
    std::wstring text;
    for(const auto& l:lines){const auto t=trimmed(l);if(t.empty())continue;if(!text.empty())text+=L' ';text+=t;}
    if(text.empty()||text.size()>o_.maxChars||lines.size()>4){
        // Nothing, or a page of text (a menu, a chat): not a subtitle.
        window_.clear();released_=false;onScreen_.reset();
        return std::nullopt;
    }
    if(released_){
        if(sameLine(text,screenText_)||misreading(text))return std::nullopt; // the same subtitle still shown
        released_=false;onScreen_.reset();window_.clear();
    }
    if(window_.empty())appeared_=now; // a line fading or typing in keeps its first moment
    if(fresh||window_.empty())window_.push_back({now,now,lines,text,1,name});
    else{window_.back().last=now;++window_.back().times;}
    window_.erase(std::remove_if(window_.begin(),window_.end(),[&](const Reading& r){return now-r.last>1000;}),window_.end());

    auto group=agreeing(text);
    if(group.empty())return std::nullopt;
    size_t longest=0;for(const auto& r:window_)longest=std::max(longest,plain(r.text).size());
    if(longest>plain(group.front()->text).size()+2&&now-appeared_<500)return std::nullopt; // a fuller reading may still settle
    // A fuller reading seen just as steadily: the shorter one lost a piece.
    for(const auto& r:window_){
        if(plain(r.text).size()>plain(group.front()->text).size()+2){
            auto fuller=agreeing(r.text);
            if(!fuller.empty())group=std::move(fuller);
        }
    }
    const auto chosen=best(group);
    const auto coloured=colouredName(group);
    released_=true;
    screenText_=joined(chosen);
    window_.clear();
    while(!recent_.empty()&&now-recent_.front().first>o_.memoryMs)recent_.pop_front();
    Split s;
    if(!coloured.empty()){
        s={coloured,joined(chosen)}; // the name stood apart in its colour: the line is all speech
        if(std::find(colouredNames_.begin(),colouredNames_.end(),coloured)==colouredNames_.end())colouredNames_.push_back(coloured);
    }else if(!colouredNames_.empty())s=namedBefore(chosen);
    else s=splitSpeaker(chosen,o_.names);
    if(s.speaker.empty()&&o_.names!=NameStyle::None)s=knownPrefix(s.speech);
    if(!s.speaker.empty())s.speaker=canonical(s.speaker);
    auto speech=withoutBracketed(s.speech); // after the name: "[Name] text" names the speaker
    speech=withoutNames(speech);
    speech=onlyNew(o_.russian?fixMisreads(speech,d_):squeeze(speech));
    if(speech.empty()||plain(speech).size()<2)return std::nullopt;
    recent_.push_back({now,speech});
    onScreen_=SpokenLine{s.speaker,speech,appeared_};
    return onScreen_;
}

void SubtitleTracker::reset(){
    window_.clear();appeared_=0;released_=false;screenText_.clear();onScreen_.reset();
    speakers_.clear();colouredNames_.clear();recent_.clear();waiting_.clear();
}

std::optional<SpokenLine> SubtitleTracker::updateParts(const std::vector<std::pair<std::wstring,std::wstring>>& parts,int64_t now){
    while(!recent_.empty()&&now-recent_.front().first>o_.memoryMs)recent_.pop_front();
    std::optional<std::wstring> shown; // a part spoken already and still on screen
    for(const auto& [name,said0]:parts){
        auto said=withoutBracketed(said0);
        said=o_.russian?fixMisreads(said,d_):squeeze(said);
        if(plain(said).size()<2)continue;
        if(std::any_of(recent_.begin(),recent_.end(),[&](const auto& r){return similar(said,r.second);})){shown=said;continue;}
        auto it=std::find_if(waiting_.begin(),waiting_.end(),[&](const Waiting& w){return sameLine(said,w.readings.back());});
        if(it!=waiting_.end()){it->last=now;it->readings.push_back(said);}
        else waiting_.push_back({name,now,now,{said}});
    }
    waiting_.erase(std::remove_if(waiting_.begin(),waiting_.end(),[&](const Waiting& w){return now-w.last>1000;}),waiting_.end());
    if(onScreen_&&(!shown||!similar(*shown,onScreen_->speech)))onScreen_.reset(); // the last spoken line has gone
    for(size_t k=0;k<waiting_.size();){
        if(waiting_[k].readings.size()<2||waiting_[k].last-waiting_[k].first<o_.settleMs){++k;continue;}
        const Waiting w=std::move(waiting_[k]);
        waiting_.erase(waiting_.begin()+long(k));
        // Real words first, then agreement with the other readings, then the latest.
        size_t pick=0;double bestWords=-1,bestAgree=-1;
        for(size_t i=0;i<w.readings.size();++i){
            const double words=o_.russian?realWords(w.readings[i],d_.russian):0.0;
            double agree=0;for(const auto& r:w.readings)agree+=ratio(w.readings[i],r);
            if(words>bestWords||(words==bestWords&&agree>=bestAgree)){bestWords=words;bestAgree=agree;pick=i;}
        }
        const auto speech=onlyNew(w.readings[pick]);
        if(plain(speech).size()<2)continue; // all of it said already
        auto speaker=trimmed(w.name);
        while(!speaker.empty()&&(speaker.back()==L':'||speaker.back()==L'：'))speaker.pop_back();
        speaker=cleanName(speaker);
        if(o_.russian)speaker=nameLetters(speaker);
        speaker=!speaker.empty()&&looksLikeName(speaker,true)?canonical(speaker):std::wstring{};
        recent_.push_back({now,speech});
        onScreen_=SpokenLine{speaker,speech,w.first};
        return onScreen_;
    }
    return std::nullopt;
}

// The readings of this same line, if seen at least twice over the settle time (not necessarily
// in a row: one stray reading between two good ones does not restart the wait).
std::vector<const SubtitleTracker::Reading*> SubtitleTracker::agreeing(const std::wstring& text)const{
    std::vector<const Reading*> group;int times=0;
    for(const auto& r:window_)if(sameLine(r.text,text)){group.push_back(&r);times+=r.times;}
    if(group.empty()||times<2||group.back()->last-group.front()->first<o_.settleMs)return {};
    return group;
}

// The most trustworthy reading: misread letters rarely repeat as the scene behind changes, so
// readings of real words first, then the one most like all the others.
std::vector<std::wstring> SubtitleTracker::best(const std::vector<const Reading*>& group)const{
    if(group.size()==1)return group.front()->lines;
    std::vector<std::wstring> texts;
    for(const auto* r:group){std::wstring t;for(size_t i=0;i<r->lines.size();++i){if(i)t+=L' ';t+=r->lines[i];}texts.push_back(t);}
    std::vector<double> words(texts.size(),0.0);
    if(o_.russian)for(size_t i=0;i<texts.size();++i)words[i]=realWords(texts[i],d_.russian);
    const double top=*std::max_element(words.begin(),words.end());
    size_t pick=0;double bestAgree=-1;
    for(size_t i=0;i<texts.size();++i){
        if(words[i]<top-1e-9)continue;
        double agree=0;for(const auto& t:texts)agree+=ratio(texts[i],t);
        if(agree>=bestAgree){bestAgree=agree;pick=i;}
    }
    return group[pick]->lines;
}

// The coloured name by vote across the readings (letters only: plates have colours too).
std::wstring SubtitleTracker::colouredName(const std::vector<const Reading*>& group)const{
    std::vector<std::pair<std::wstring,int>> votes;
    for(const auto* r:group){
        auto n=trimmed(r->name);
        while(!n.empty()&&(n.back()==L':'||n.back()==L'：'))n.pop_back();
        n=cleanName(n);
        if(o_.russian)n=nameLetters(n);
        if(n.empty()||!looksLikeName(n,true)||std::any_of(n.begin(),n.end(),isDigit))continue;
        auto it=std::find_if(votes.begin(),votes.end(),[&](const auto& v){return v.first==n;});
        if(it==votes.end())votes.push_back({n,r->times});else it->second+=r->times;
    }
    std::wstring best;int most=0;
    for(const auto& [n,v]:votes)if(v>most){most=v;best=n;}
    return best;
}

// A line starting with a name this game has shown in colour, and a colon.
Split SubtitleTracker::namedBefore(const std::vector<std::wstring>& lines)const{
    const auto text=joined(lines);
    const auto colon=text.find(L':');
    if(colon!=std::wstring::npos){
        const auto head=cleanName(text.substr(0,colon)),rest=trimmed(text.substr(colon+1));
        if(!rest.empty()&&std::any_of(colouredNames_.begin(),colouredNames_.end(),[&](const auto& n){return similar(head,n,0.85);}))
            return {head,rest};
    }
    return splitSpeaker(lines,o_.names);
}

// A known name at the start whose colon was misread ("Анна; ...").
Split SubtitleTracker::knownPrefix(const std::wstring& speech)const{
    const auto words=splitWords(speech);
    for(size_t size:{3u,2u,1u}){
        if(words.size()<=size)continue;
        std::wstring head;for(size_t i=0;i<size;++i){if(i)head+=L' ';head+=words[i];}
        if(!in(L":;.,|!",head.back()))continue;
        head.pop_back();
        if(std::any_of(speakers_.begin(),speakers_.end(),[&](const auto& k){return similar(head,k,0.8);})){
            std::wstring rest;for(size_t i=size;i<words.size();++i){if(!rest.empty())rest+=L' ';rest+=words[i];}
            return {head,rest};
        }
    }
    return {{},speech};
}

// Without the coloured names (with their colons) further on in the line: a second speaker.
std::wstring SubtitleTracker::withoutNames(std::wstring speech)const{
    auto names=colouredNames_;
    std::stable_sort(names.begin(),names.end(),[](const auto& a,const auto& b){return a.size()>b.size();});
    for(const auto& n:names){
        if(n.empty())continue;
        std::wstring o;
        for(size_t i=0;i<speech.size();){
            if(speech.compare(i,n.size(),n)==0&&(i==0||!isWordChar(speech[i-1]))){
                size_t j=i+n.size();while(j<speech.size()&&isSpace(speech[j]))++j;
                if(j<speech.size()&&(speech[j]==L':'||speech[j]==L'：')){
                    ++j;while(j<speech.size()&&isSpace(speech[j]))++j;
                    o+=L' ';i=j;continue;
                }
            }
            o+=speech[i++];
        }
        speech.swap(o);
    }
    return squeeze(speech);
}

// A slightly misread name maps onto the character already seen ("Cтapик" -> "Старик").
std::wstring SubtitleTracker::canonical(const std::wstring& name){
    for(const auto& k:speakers_)if(similar(name,k,0.8))return k;
    speakers_.push_back(name);
    return name;
}

// Without what was spoken already: a repeat, or the old start of a growing line.
std::wstring SubtitleTracker::onlyNew(const std::wstring& speech)const{
    const auto p=plain(speech);
    for(auto it=recent_.rbegin();it!=recent_.rend();++it){
        if(similar(speech,it->second))return {};
        const auto before=plain(it->second);
        if(before.size()>=8&&p.compare(0,before.size(),before)==0){
            const auto words=splitWords(speech);
            const size_t skip=splitWords(it->second).size();
            std::wstring rest;for(size_t i=skip;i<words.size();++i){if(!rest.empty())rest+=L' ';rest+=words[i];}
            return rest;
        }
    }
    return speech;
}

bool SubtitleTracker::misreading(const std::wstring& text)const{
    return similar(text,screenText_,0.7)&&long(plain(text).size())-long(plain(screenText_).size())<=6;
}

} // namespace veyra::engine::screentext
