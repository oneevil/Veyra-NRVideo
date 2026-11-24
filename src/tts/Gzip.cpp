// SPDX-License-Identifier: GPL-3.0-only
#include "Gzip.h"

#include <array>
#include <cstdint>

namespace veyra::tts {
namespace {

class BitReader {
public:
    explicit BitReader(std::string_view data) : data_(data) {}
    // -1 when the input ran out.
    int bits(int count) {
        uint32_t value = 0;
        for (int i = 0; i < count; ++i) {
            if (!have_) {
                if (pos_ >= data_.size()) return -1;
                byte_ = uint8_t(data_[pos_++]);
                have_ = 8;
            }
            value |= uint32_t(byte_ & 1) << i;
            byte_ >>= 1;
            --have_;
        }
        return int(value);
    }
    void alignToByte() { have_ = 0; }
    size_t position() const { return pos_; }
    void skip(size_t bytes) { pos_ += bytes; }
    std::string_view data() const { return data_; }

private:
    std::string_view data_;
    size_t pos_ = 0;
    uint8_t byte_ = 0;
    int have_ = 0;
};

// Canonical Huffman code: counts of codes per length and the symbols in code order.
struct Huffman {
    std::array<uint16_t, 16> count{};
    std::array<uint16_t, 320> symbol{};

    bool build(const uint8_t* lengths, int n) {
        count.fill(0);
        for (int i = 0; i < n; ++i) ++count[lengths[i]];
        if (count[0] == n) return true; // no codes: valid only if never used
        int left = 1;
        for (int len = 1; len < 16; ++len) {
            left <<= 1;
            left -= count[len];
            if (left < 0) return false; // over-subscribed
        }
        std::array<uint16_t, 16> offset{};
        for (int len = 1; len < 15; ++len) offset[len + 1] = uint16_t(offset[len] + count[len]);
        for (int i = 0; i < n; ++i)
            if (lengths[i]) symbol[offset[lengths[i]]++] = uint16_t(i);
        return true;
    }

    int decode(BitReader& in) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            const int bit = in.bits(1);
            if (bit < 0) return -1;
            code |= bit;
            const int c = count[len];
            if (code - c < first) return symbol[index + (code - first)];
            index += c;
            first += c;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

constexpr uint16_t lengthBase[29]{3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                  35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t lengthExtra[29]{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t distBase[30]{1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t distExtra[30]{0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool inflateCodes(BitReader& in, std::string& out, const Huffman& lengths, const Huffman& distances) {
    for (;;) {
        int symbol = lengths.decode(in);
        if (symbol < 0) return false;
        if (symbol < 256) {
            out.push_back(char(symbol));
            continue;
        }
        if (symbol == 256) return true;
        symbol -= 257;
        if (symbol >= 29) return false;
        const int extra = in.bits(lengthExtra[symbol]);
        if (extra < 0) return false;
        const size_t length = lengthBase[symbol] + extra;
        const int d = distances.decode(in);
        if (d < 0 || d >= 30) return false;
        const int dExtra = in.bits(distExtra[d]);
        if (dExtra < 0) return false;
        const size_t distance = distBase[d] + dExtra;
        if (distance > out.size()) return false;
        const size_t from = out.size() - distance;
        for (size_t i = 0; i < length; ++i) out.push_back(out[from + i]);
    }
}

bool inflateFixed(BitReader& in, std::string& out) {
    static const auto tables = [] {
        std::array<uint8_t, 320> lengths{};
        int i = 0;
        for (; i < 144; ++i) lengths[i] = 8;
        for (; i < 256; ++i) lengths[i] = 9;
        for (; i < 280; ++i) lengths[i] = 7;
        for (; i < 288; ++i) lengths[i] = 8;
        std::pair<Huffman, Huffman> result;
        result.first.build(lengths.data(), 288);
        std::array<uint8_t, 30> dist{};
        dist.fill(5);
        result.second.build(dist.data(), 30);
        return result;
    }();
    return inflateCodes(in, out, tables.first, tables.second);
}

bool inflateDynamic(BitReader& in, std::string& out) {
    const int nlen = in.bits(5), ndist = in.bits(5), ncode = in.bits(4);
    if (nlen < 0 || ndist < 0 || ncode < 0) return false;
    const int lengthCount = nlen + 257, distCount = ndist + 1, codeCount = ncode + 4;
    if (lengthCount > 286 || distCount > 30) return false;
    static constexpr uint8_t order[19]{16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    std::array<uint8_t, 320> lengths{};
    for (int i = 0; i < codeCount; ++i) {
        const int value = in.bits(3);
        if (value < 0) return false;
        lengths[order[i]] = uint8_t(value);
    }
    Huffman codes;
    if (!codes.build(lengths.data(), 19)) return false;
    lengths.fill(0);
    for (int index = 0; index < lengthCount + distCount;) {
        const int symbol = codes.decode(in);
        if (symbol < 0) return false;
        if (symbol < 16) {
            lengths[index++] = uint8_t(symbol);
            continue;
        }
        uint8_t value = 0;
        int repeat = 0;
        if (symbol == 16) {
            if (!index) return false;
            value = lengths[index - 1];
            repeat = in.bits(2);
            if (repeat < 0) return false;
            repeat += 3;
        } else if (symbol == 17) {
            repeat = in.bits(3);
            if (repeat < 0) return false;
            repeat += 3;
        } else {
            repeat = in.bits(7);
            if (repeat < 0) return false;
            repeat += 11;
        }
        if (index + repeat > lengthCount + distCount) return false;
        while (repeat--) lengths[index++] = value;
    }
    if (!lengths[256]) return false; // no end-of-block code
    Huffman literal, distance;
    if (!literal.build(lengths.data(), lengthCount)) return false;
    if (!distance.build(lengths.data() + lengthCount, distCount)) return false;
    return inflateCodes(in, out, literal, distance);
}

bool inflateStored(BitReader& in, std::string& out) {
    in.alignToByte();
    const auto data = in.data();
    const size_t at = in.position();
    if (at + 4 > data.size()) return false;
    const unsigned len = uint8_t(data[at]) | (uint8_t(data[at + 1]) << 8);
    const unsigned nlen = uint8_t(data[at + 2]) | (uint8_t(data[at + 3]) << 8);
    if (len != (~nlen & 0xFFFF) || at + 4 + len > data.size()) return false;
    out.append(data.substr(at + 4, len));
    in.skip(4 + len);
    return true;
}

uint32_t crc32(std::string_view data) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    for (const char ch : data) crc = table[(crc ^ uint8_t(ch)) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

uint32_t le32(std::string_view data, size_t at) {
    return uint32_t(uint8_t(data[at])) | uint32_t(uint8_t(data[at + 1])) << 8 |
           uint32_t(uint8_t(data[at + 2])) << 16 | uint32_t(uint8_t(data[at + 3])) << 24;
}

} // namespace

std::optional<std::string> gunzip(std::string_view gz) {
    if (gz.size() < 18 || uint8_t(gz[0]) != 0x1F || uint8_t(gz[1]) != 0x8B || gz[2] != 8) return std::nullopt;
    const uint8_t flags = uint8_t(gz[3]);
    size_t at = 10;
    if (flags & 4) { // FEXTRA
        if (at + 2 > gz.size()) return std::nullopt;
        at += 2 + (uint8_t(gz[at]) | (uint8_t(gz[at + 1]) << 8));
    }
    for (const uint8_t flag : {uint8_t(8), uint8_t(16)}) { // FNAME, FCOMMENT
        if (!(flags & flag)) continue;
        while (at < gz.size() && gz[at]) ++at;
        ++at;
    }
    if (flags & 2) at += 2; // FHCRC
    if (at >= gz.size()) return std::nullopt;

    std::string out;
    out.reserve(gz.size() * 8);
    BitReader in(gz.substr(at));
    for (bool last = false; !last;) {
        const int final = in.bits(1), type = in.bits(2);
        if (final < 0 || type < 0) return std::nullopt;
        last = final == 1;
        const bool ok = type == 0 ? inflateStored(in, out) : type == 1 ? inflateFixed(in, out)
                      : type == 2 ? inflateDynamic(in, out) : false;
        if (!ok) return std::nullopt;
    }
    const size_t trailer = at + in.position();
    if (trailer + 8 > gz.size()) return std::nullopt;
    if (le32(gz, trailer) != crc32(out) || le32(gz, trailer + 4) != uint32_t(out.size())) return std::nullopt;
    return out;
}

} // namespace veyra::tts
