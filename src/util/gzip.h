#pragma once

// Hard output cap: a hostile member that expands forever must fail loudly
// instead of filling the disk.

#include <cstddef>
#include <string>
#include <string_view>

#include "result.h"

namespace berth {

// Err on any zlib error, on trailing garbage after the final block, and when
// the output exceeds max_out.
[[nodiscard]] Result<std::string> gunzip(std::string_view compressed,
                                         std::size_t max_out);

}
