// Cookie header parsing (django.http.cookie.parse_cookie).
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace django::native {

// Parse Cookie header → ordered (key, value) pairs. Later keys overwrite
// earlier ones when building a dict (caller may use last-wins).
[[nodiscard]] std::vector<std::pair<std::string, std::string>> parse_cookie(
    std::string_view cookie);

// http.cookies._unquote
[[nodiscard]] std::string cookie_unquote(std::string_view value);

}  // namespace django::native
