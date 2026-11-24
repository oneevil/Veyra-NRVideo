// SPDX-License-Identifier: GPL-3.0-only
#include "WordList.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace veyra::engine::screentext {
namespace {
constexpr char magic[8]={'V','T','S','C','A','C','H','1'};
// The same hash as tts::wordHash (FNV-1a over the four bytes of each code point).
uint64_t hashOf(std::wstring_view word){
    uint64_t hash=1469598103934665603ull;
    for(const wchar_t c:word){
        const auto code=uint32_t(c);
        for(int shift=0;shift<32;shift+=8){hash^=uint64_t((code>>shift)&0xFF);hash*=1099511628211ull;}
    }
    return hash;
}
}

bool WordList::load(const std::filesystem::path& file){
    keys_.clear();
    std::ifstream in(file,std::ios::binary);
    char head[8]{};
    uint64_t count=0;
    if(!in.read(head,8)||std::memcmp(head,magic,8)||!in.read(reinterpret_cast<char*>(&count),8))return false;
    if(count==0||count>20000000)return false;
    keys_.resize(count);
    if(!in.read(reinterpret_cast<char*>(keys_.data()),std::streamsize(count*8))||!std::is_sorted(keys_.begin(),keys_.end())){
        keys_.clear();
        return false;
    }
    return true;
}

bool WordList::known(std::wstring_view lower)const{
    if(keys_.empty()||lower.empty())return false;
    const uint64_t h=hashOf(lower);
    if(std::binary_search(keys_.begin(),keys_.end(),h))return true;
    // the dictionary writes "е" where the text may have "ё"
    if(lower.find(L'ё')==std::wstring_view::npos)return false;
    std::wstring plain(lower);
    std::replace(plain.begin(),plain.end(),L'ё',L'е');
    return std::binary_search(keys_.begin(),keys_.end(),hashOf(plain));
}

} // namespace veyra::engine::screentext
