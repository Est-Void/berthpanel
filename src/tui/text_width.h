#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace tui {

std::size_t display_width(std::string_view text);

std::string truncate_to_width(
    std::string_view text, std::size_t width,
    std::string_view ellipsis = "\xE2\x80\xA6");

}
