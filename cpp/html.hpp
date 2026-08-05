// HTML / JS string escaping (matches django.utils.html semantics).
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace django::native {

// Match Python html.escape(s, quote=True):
//   & → &amp;   < → &lt;   > → &gt;   " → &quot;   ' → &#x27;
[[nodiscard]] std::string html_escape(std::string_view input);

// Match django.utils.html.escapejs (Unicode code-point translation).
[[nodiscard]] std::string escapejs(std::string_view input);

// Convert newlines into <p> / <br> (django.utils.html.linebreaks).
// When autoescape is true, paragraph text is HTML-escaped first.
[[nodiscard]] std::string linebreaks(std::string_view value, bool autoescape);

// Normalize newlines then replace \n with <br> (optionally escape first).
[[nodiscard]] std::string linebreaksbr(std::string_view value, bool autoescape);

// One pass of MLStripper-like tag removal (django.utils.html._strip_once).
[[nodiscard]] std::string strip_tags_once(std::string_view value);

// Full strip_tags loop. Throws std::runtime_error("SuspiciousOperation") when
// depth exceeds MAX_STRIP_TAGS_DEPTH (50).
[[nodiscard]] std::string strip_tags(std::string_view value);

// HTML-aware truncate matching TruncateCharsHTMLParser / TruncateWordsHTMLParser
// for well-formed markup. `truncate_suffix` is the already-resolved suffix (e.g. "…").
// `length` is the user-facing char/word limit (suffix length is subtracted for chars).
[[nodiscard]] std::string truncate_chars_html(std::string_view html, int length,
                                              std::string_view truncate_suffix);
[[nodiscard]] std::string truncate_words_html(std::string_view html, int length,
                                              std::string_view truncate_suffix);

// IPv6 clean: compress longest zero run, lowercase, optional IPv4-mapped unpack.
// Returns nullopt if invalid.
[[nodiscard]] std::optional<std::string> clean_ipv6_address(std::string_view ip,
                                                            bool unpack_ipv4,
                                                            int max_length = 39);

}  // namespace django::native
