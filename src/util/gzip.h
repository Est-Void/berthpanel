#pragma once

// src/util/gzip.h — gzip decompression via system zlib (dev.md §2), with a
// hard output cap. Archives are never trusted: a hostile gzip member that
// expands forever must fail loudly instead of filling the disk, which is
// the "bomb" case a length check alone would miss.

#include <cstddef>
#include <string>
#include <string_view>

#include "result.h"

namespace berth {

// Inflates a whole gzip stream. Err on any zlib error, on trailing garbage
// after the final block, and when the output exceeds max_out.
[[nodiscard]] Result<std::string> gunzip(std::string_view compressed,
                                         std::size_t max_out);

}  // namespace berth