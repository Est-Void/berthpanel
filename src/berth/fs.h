#pragma once

#include <filesystem>

#include "../util/result.h"

namespace berth {

[[nodiscard]] Result<std::size_t> remove_tree(const std::filesystem::path& root);

}  // namespace berth
