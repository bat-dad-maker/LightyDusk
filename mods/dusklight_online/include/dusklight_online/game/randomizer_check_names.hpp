#pragma once

#include <string_view>

namespace dusklight_online::game {

// Returns an empty view when a technical check ID has no known display name.
std::string_view randomizer_check_display_name(std::string_view checkName);

}  // namespace dusklight_online::game
