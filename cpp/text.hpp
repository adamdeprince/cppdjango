// Text utilities (slugify, get_valid_filename).
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace django::native {

// Core slugify after Unicode normalization has been applied in Python
// (NFKC + lower for allow_unicode, NFKD→ASCII for ASCII mode).
// Performs: lowercase (ASCII mode only), strip non [word/space/hyphen],
// collapse runs of spaces/hyphens, strip leading/trailing - and _.
[[nodiscard]] std::string slugify_core(std::string_view value, bool allow_unicode);

// Sanitize a filename. Returns nullopt when the result would be "", ".", or "..".
[[nodiscard]] std::optional<std::string> get_valid_filename(std::string_view name);

// Truncator plain-text paths (html=False). `truncate` is the suffix (e.g. "…").
// Combining characters (UTF-8 combining marks) are not counted toward length,
// matching unicodedata.combining for common marks (approx. Mn category ranges).
[[nodiscard]] std::string truncate_chars(std::string_view text, int length,
                                         std::string_view truncate_suffix);
[[nodiscard]] std::string truncate_words(std::string_view text, int length,
                                         std::string_view truncate_suffix);

// QueryDict.urlencode from list of (key, value) pairs.
// safe is URL-quote safe characters (may be empty).
[[nodiscard]] std::string querydict_urlencode(
    const std::vector<std::pair<std::string, std::string>>& pairs,
    std::string_view safe);

}  // namespace django::native
