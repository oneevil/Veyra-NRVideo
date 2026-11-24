// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Reading a .gz file (RFC 1952 around RFC 1951 deflate): the stress dictionaries of the voice
// are published gzip-compressed, and the build has no zlib of its own to link.
#include <optional>
#include <string>
#include <string_view>

namespace veyra::tts {

// The decompressed bytes, or nothing if the data is not a valid gzip member (the CRC and the
// length in the trailer are checked).
std::optional<std::string> gunzip(std::string_view compressed);

} // namespace veyra::tts
