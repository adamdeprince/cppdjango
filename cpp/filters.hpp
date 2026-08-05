// Hot template defaultfilters (pure string ops).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace django::native {

[[nodiscard]] std::string filter_addslashes(std::string_view value);
[[nodiscard]] std::string filter_capfirst(std::string_view value);
[[nodiscard]] std::string filter_lower(std::string_view value);  // ASCII-oriented
[[nodiscard]] std::string filter_upper(std::string_view value);  // ASCII-oriented
[[nodiscard]] std::string filter_cut(std::string_view value, std::string_view arg);
[[nodiscard]] std::size_t filter_wordcount(std::string_view value);
[[nodiscard]] std::string filter_ljust(std::string_view value, int width);
[[nodiscard]] std::string filter_rjust(std::string_view value, int width);
[[nodiscard]] std::string filter_center(std::string_view value, int width);
// ASCII-oriented str.title() + Django apostrophe/digit post-process.
[[nodiscard]] std::string filter_title(std::string_view value);
// Apply Python-style slice to a string. nullopt start/stop/step = omitted.
[[nodiscard]] std::optional<std::string> filter_slice_string(
    std::string_view value, std::optional<int> start, std::optional<int> stop,
    std::optional<int> step);

// URL quoting for reverse() — quote(s, safe=RFC3986_SUBDELIMS + "/~:@")
[[nodiscard]] std::string url_quote(std::string_view value, std::string_view safe);

// escape_leading_slashes: "//foo" → "/%2Ffoo"
[[nodiscard]] std::string escape_leading_slashes(std::string_view url);

// More utils used by templates / text processing.
[[nodiscard]] std::string phone2numeric(std::string_view phone);
[[nodiscard]] std::string normalize_newlines(std::string_view text);
[[nodiscard]] std::string strip_spaces_between_tags(std::string_view value);
[[nodiscard]] std::string camel_case_to_spaces(std::string_view value);
[[nodiscard]] std::string pluralize_suffix(bool singular, std::string_view arg);
[[nodiscard]] std::string yesno(int tri_state, std::string_view arg);
// tri_state: 1=true, 0=false, -1=None
[[nodiscard]] int get_digit(std::int64_t value, int arg);
[[nodiscard]] std::string widthratio(double value, double max_value, int max_width);
[[nodiscard]] std::pair<std::string, std::string> get_mod_func(std::string_view callback);
[[nodiscard]] std::string iri_to_uri(std::string_view iri);
[[nodiscard]] std::string uri_to_iri(std::string_view uri);
[[nodiscard]] std::string escape_uri_path(std::string_view path);
[[nodiscard]] std::string filepath_to_uri(std::string_view path);
[[nodiscard]] bool divisibleby(std::int64_t value, std::int64_t arg);
// Integer add; nullopt if overflow (caller may try other paths).
[[nodiscard]] std::optional<std::int64_t> filter_add_int(std::int64_t value,
                                                         std::int64_t arg);

// UTF-8 code-point count (Python len(str)).
[[nodiscard]] std::size_t utf8_length(std::string_view value);
// First/last Unicode character as UTF-8; empty string if empty.
[[nodiscard]] std::string utf8_first(std::string_view value);
[[nodiscard]] std::string utf8_last(std::string_view value);
// list(str) → sequence of Unicode characters (UTF-8 strings).
[[nodiscard]] std::vector<std::string> make_list_chars(std::string_view value);
// linenumbers filter core (lines already split on \n). autoescape applies
// html_escape per line when true.
[[nodiscard]] std::string linenumbers(std::string_view value, bool autoescape);
// django.utils.text.wrap — word wrap preserving newlines.
[[nodiscard]] std::string wordwrap(std::string_view text, int width);

// Join pre-stringified parts (escape handled in Python).
[[nodiscard]] std::string join_strings(const std::vector<std::string>& parts,
                                       std::string_view sep);

}  // namespace django::native
