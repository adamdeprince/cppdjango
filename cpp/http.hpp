// HTTP helpers (query-string parsing for QueryDict).
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace django::native {

// Match urllib.parse.parse_qsl for UTF-8 with keep_blank_values=True,
// strict_parsing=False, separator='&'.
//
// Raises std::invalid_argument when max_num_fields is exceeded (same counting
// rule as CPython: 1 + count('&')).
[[nodiscard]] std::vector<std::pair<std::string, std::string>> parse_qsl_utf8(
    std::string_view qs, std::optional<std::size_t> max_num_fields);

}  // namespace django::native
