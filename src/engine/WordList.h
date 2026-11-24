#pragma once
// Whether a word is a real Russian word, for choosing among readings of a subtitle: the word
// list of the stress dictionary (RUAccent, 3.3 million forms), read from the quick-to-load copy
// the voice writes on its first start (voices/cache/teratts-accents.bin: sorted 64-bit hashes of
// the words, src/tts/RuStress). Without that file nothing is known and the reader manages without.
// Portable C++.
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace veyra::engine::screentext {

class WordList {
public:
    bool load(const std::filesystem::path& file); // false: not there or not that format
    bool loaded()const{return !keys_.empty();}
    bool known(std::wstring_view lower)const;      // a lower-case word
private:
    std::vector<uint64_t> keys_;
};

} // namespace veyra::engine::screentext
