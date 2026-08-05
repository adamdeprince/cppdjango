// Urlize helpers + HTML-aware truncate (simplified streaming strip).
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace django::native {

// Urlizer.trim_url
[[nodiscard]] std::string trim_url(std::string_view url, int limit);

// Split like word_split_re: ([\s<>"']+)
[[nodiscard]] std::vector<std::string> urlize_word_split(std::string_view text);

// Trim lead/middle/trail punctuation (simplified: strip wrapping ()[] and
// trailing .,;:! balancing).
struct TrimPunct {
  std::string lead;
  std::string middle;
  std::string trail;
};
[[nodiscard]] TrimPunct trim_urlize_punctuation(std::string_view word);

// Plain is_email_simple: uses structural email check (no allowlist).
[[nodiscard]] bool urlize_is_email_simple(std::string_view value);

// Urlizer.simple_url_re: ^https?://\[?\w  (case-insensitive)
[[nodiscard]] bool urlize_simple_url_match(std::string_view middle) noexcept;

// Urlizer.simple_url_2_re: www. … or bare domain with classic gTLD (.com etc.)
// Matches structural form used by Django (hostname labels + gTLD + optional path).
[[nodiscard]] bool urlize_simple_url_2_match(std::string_view middle) noexcept;

// Decimal digit accounting for DecimalValidator.
// digits: concatenation of digit_tuple as chars '0'-'9'
// exponent: Decimal as_tuple exponent (negative = decimal places).
// Returns {total_digits, decimals, whole_digits}; invalid if exponent special.
struct DecimalDigitCounts {
  bool invalid = false;
  int digits = 0;
  int decimals = 0;
  int whole_digits = 0;
};
[[nodiscard]] DecimalDigitCounts decimal_digit_counts(std::string_view digits,
                                                      int exponent);

// sanitize_separators for known decimal/thousand seps (ASCII-oriented).
[[nodiscard]] std::string sanitize_separators_ascii(std::string_view value,
                                                    std::string_view decimal_sep,
                                                    std::string_view thousand_sep,
                                                    bool use_thousand);

// QueryDict urlencode for already-encoded (latin-1 etc.) key/value byte pairs
// represented as latin-1 strings (each char is a byte 0-255).
[[nodiscard]] std::string querydict_urlencode_bytes(
    const std::vector<std::pair<std::string, std::string>>& pairs,
    std::string_view safe);

}  // namespace django::native
