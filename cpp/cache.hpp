// Cache-Control / Vary / conditional request helpers (django.utils.cache).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace django::native {

// Split on \\s*,\\s* (cc_delim_re).
[[nodiscard]] std::vector<std::string> cc_delim_split(std::string_view header);

// Parse Cache-Control into (directive_lower, value). Empty value means boolean
// True (directive present without '=').
[[nodiscard]] std::vector<std::pair<std::string, std::string>> parse_cache_control(
    std::string_view header);

// Extract max-age integer, or nullopt.
[[nodiscard]] std::optional<int> get_max_age_from_cc(std::string_view header);

// RFC 9110 §13.1.1 / §13.1.2 If-Match / If-None-Match.
[[nodiscard]] bool if_match_passes(std::string_view target_etag,
                                   const std::vector<std::string>& etags);
[[nodiscard]] bool if_none_match_passes(std::string_view target_etag,
                                        const std::vector<std::string>& etags);

// Date comparisons (unix seconds). last_modified nullopt = missing.
[[nodiscard]] bool if_unmodified_since_passes(std::optional<std::int64_t> last_modified,
                                              std::int64_t if_unmodified_since);
[[nodiscard]] bool if_modified_since_passes(std::optional<std::int64_t> last_modified,
                                            std::int64_t if_modified_since);

// patch_vary_headers core: existing Vary value + new header names → new value.
[[nodiscard]] std::string patch_vary_headers(std::string_view existing_vary,
                                             const std::vector<std::string>& newheaders);

[[nodiscard]] bool has_vary_header(std::string_view vary_header,
                                   std::string_view header_query);

// Merge kwargs into an existing Cache-Control header string.
// kwargs: list of (name_with_underscores_or_hyphens, value_string, is_bool_true).
// When is_bool_true, only the directive name is emitted.
// For max-age, value is decimal integer string; min with existing applied.
[[nodiscard]] std::string merge_cache_control(
    std::string_view existing_header,
    const std::vector<std::tuple<std::string, std::string, bool>>& kwargs);

}  // namespace django::native
