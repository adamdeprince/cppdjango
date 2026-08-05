#include "text.hpp"

#include "filters.hpp"  // url_quote

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace django::native {
namespace {

[[nodiscard]] bool is_ascii_alnum(char32_t c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

[[nodiscard]] bool is_ascii_space(char32_t c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// Common Unicode space separators (Python \s subset for slugify inputs).
[[nodiscard]] bool is_unicode_space(char32_t c) noexcept {
  if (is_ascii_space(c)) {
    return true;
  }
  return c == 0x00A0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 ||
         c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

// Combining marks — dropped by Python's [^\w\s-] because Mn is not \w.
[[nodiscard]] bool is_combining_mark(char32_t c) noexcept {
  return (c >= 0x0300 && c <= 0x036F) || (c >= 0x1AB0 && c <= 0x1AFF) ||
         (c >= 0x1DC0 && c <= 0x1DFF) || (c >= 0x20D0 && c <= 0x20FF) ||
         (c >= 0xFE20 && c <= 0xFE2F);
}

// Approximate Python re \w for slugify/filename after lowercasing.
// ASCII: [A-Za-z0-9_]; non-ASCII: keep unless combining mark or known punct.
[[nodiscard]] bool is_word_char(char32_t c, bool allow_unicode) noexcept {
  if (c == '_') {
    return true;
  }
  if (c < 0x80) {
    return is_ascii_alnum(c);
  }
  if (!allow_unicode) {
    return false;
  }
  if (is_combining_mark(c)) {
    return false;
  }
  // Drop common Unicode punctuation / symbols that Python \w excludes.
  // General Punctuation, etc. — keep letters (Lo/Ll/…) and digits.
  if ((c >= 0x2000 && c <= 0x206F) ||  // General Punctuation
      (c >= 0x3000 && c <= 0x303F) ||  // CJK Symbols and Punctuation (excl. ideographs)
      (c >= 0xFF00 && c <= 0xFF0F) || (c >= 0xFF1A && c <= 0xFF20) ||
      (c >= 0xFF3B && c <= 0xFF40) || (c >= 0xFF5B && c <= 0xFF65)) {
    // CJK Symbols block includes U+3000 ideographic space — handled as space.
    if (c == 0x3000) {
      return false;
    }
    // U+3000–U+303F has some letters? Mostly punctuation. Safe to drop.
    if (c >= 0x3000 && c <= 0x303F) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] char32_t decode_utf8(std::string_view in, std::size_t& i) {
  const auto lead = static_cast<unsigned char>(in[i]);
  if (lead < 0x80) {
    ++i;
    return lead;
  }
  if ((lead & 0xE0) == 0xC0 && i + 1 < in.size()) {
    const auto b1 = static_cast<unsigned char>(in[i + 1]);
    if ((b1 & 0xC0) == 0x80) {
      i += 2;
      return (static_cast<char32_t>(lead & 0x1F) << 6) | (b1 & 0x3F);
    }
  } else if ((lead & 0xF0) == 0xE0 && i + 2 < in.size()) {
    const auto b1 = static_cast<unsigned char>(in[i + 1]);
    const auto b2 = static_cast<unsigned char>(in[i + 2]);
    if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80) {
      i += 3;
      return (static_cast<char32_t>(lead & 0x0F) << 12) |
             (static_cast<char32_t>(b1 & 0x3F) << 6) | (b2 & 0x3F);
    }
  } else if ((lead & 0xF8) == 0xF0 && i + 3 < in.size()) {
    const auto b1 = static_cast<unsigned char>(in[i + 1]);
    const auto b2 = static_cast<unsigned char>(in[i + 2]);
    const auto b3 = static_cast<unsigned char>(in[i + 3]);
    if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80 && (b3 & 0xC0) == 0x80) {
      i += 4;
      return (static_cast<char32_t>(lead & 0x07) << 18) |
             (static_cast<char32_t>(b1 & 0x3F) << 12) |
             (static_cast<char32_t>(b2 & 0x3F) << 6) | (b3 & 0x3F);
    }
  }
  ++i;
  return 0xFFFD;
}

void append_utf8(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

[[nodiscard]] char32_t ascii_lower(char32_t c) noexcept {
  if (c >= 'A' && c <= 'Z') {
    return c + ('a' - 'A');
  }
  return c;
}

// Strip leading/trailing '-' and '_' (ASCII only, as in Django).
void strip_dash_underscore(std::string& s) {
  std::size_t start = 0;
  while (start < s.size() && (s[start] == '-' || s[start] == '_')) {
    ++start;
  }
  std::size_t end = s.size();
  while (end > start && (s[end - 1] == '-' || s[end - 1] == '_')) {
    --end;
  }
  if (start == 0 && end == s.size()) {
    return;
  }
  s = s.substr(start, end - start);
}

}  // namespace

std::string slugify_core(std::string_view value, bool allow_unicode) {
  // Pass 1: keep word / space / hyphen; lowercase in ASCII mode.
  std::string filtered;
  filtered.reserve(value.size());

  std::size_t i = 0;
  while (i < value.size()) {
    char32_t cp = decode_utf8(value, i);
    if (!allow_unicode) {
      cp = ascii_lower(cp);
    }
    if (is_word_char(cp, allow_unicode) || is_unicode_space(cp) || cp == '-') {
      append_utf8(filtered, cp);
    }
  }

  // Pass 2: collapse runs of hyphens and whitespace to a single '-'.
  std::string out;
  out.reserve(filtered.size());
  bool in_run = false;
  i = 0;
  while (i < filtered.size()) {
    const char32_t cp = decode_utf8(filtered, i);
    if (cp == '-' || is_unicode_space(cp)) {
      if (!in_run) {
        out += '-';
        in_run = true;
      }
    } else {
      in_run = false;
      append_utf8(out, cp);
    }
  }

  strip_dash_underscore(out);
  return out;
}

std::optional<std::string> get_valid_filename(std::string_view name) {
  // strip() — trim Unicode whitespace ends.
  std::size_t start = 0;
  std::size_t end = name.size();
  auto is_strip_space = [](char32_t c) {
    return is_unicode_space(c);
  };

  // Find start
  {
    std::size_t i = 0;
    while (i < name.size()) {
      const std::size_t before = i;
      const char32_t cp = decode_utf8(name, i);
      if (!is_strip_space(cp)) {
        start = before;
        break;
      }
      start = i;
    }
    if (start >= name.size()) {
      return std::nullopt;
    }
  }
  // Find end
  {
    // Walk all codepoints to find last non-space.
    std::size_t i = start;
    std::size_t last_non_space_end = start;
    while (i < name.size()) {
      const std::size_t before = i;
      const char32_t cp = decode_utf8(name, i);
      if (!is_strip_space(cp)) {
        last_non_space_end = i;
        (void)before;
      }
    }
    end = last_non_space_end;
  }

  std::string out;
  out.reserve(end - start);
  std::size_t i = start;
  while (i < end) {
    const char32_t cp = decode_utf8(name, i);
    if (cp == ' ') {
      out += '_';
      continue;
    }
    // Keep - \w .
    if (cp == '-' || cp == '.' || is_word_char(cp, /*allow_unicode=*/true)) {
      append_utf8(out, cp);
    }
  }

  if (out.empty() || out == "." || out == "..") {
    return std::nullopt;
  }
  return out;
}

namespace {

// UTF-8 codepoint walk; returns bytes consumed; on invalid, consume 1.
std::size_t utf8_next(std::string_view s, std::size_t i, char32_t& cp) noexcept {
  if (i >= s.size()) {
    cp = 0;
    return 0;
  }
  const auto lead = static_cast<unsigned char>(s[i]);
  if (lead < 0x80) {
    cp = lead;
    return 1;
  }
  if ((lead & 0xE0) == 0xC0 && i + 1 < s.size()) {
    cp = (static_cast<char32_t>(lead & 0x1F) << 6) |
         (static_cast<unsigned char>(s[i + 1]) & 0x3F);
    return 2;
  }
  if ((lead & 0xF0) == 0xE0 && i + 2 < s.size()) {
    cp = (static_cast<char32_t>(lead & 0x0F) << 12) |
         ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
         (static_cast<unsigned char>(s[i + 2]) & 0x3F);
    return 3;
  }
  if ((lead & 0xF8) == 0xF0 && i + 3 < s.size()) {
    cp = (static_cast<char32_t>(lead & 0x07) << 18) |
         ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
         ((static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) |
         (static_cast<unsigned char>(s[i + 3]) & 0x3F);
    return 4;
  }
  cp = 0xFFFD;
  return 1;
}

// Approximate unicodedata.combining() for common mark ranges.
bool is_combining(char32_t cp) noexcept {
  // Combining Diacritical Marks and common extension blocks.
  if (cp >= 0x0300 && cp <= 0x036F) {
    return true;
  }
  if (cp >= 0x1AB0 && cp <= 0x1AFF) {
    return true;
  }
  if (cp >= 0x1DC0 && cp <= 0x1DFF) {
    return true;
  }
  if (cp >= 0x20D0 && cp <= 0x20FF) {
    return true;
  }
  if (cp >= 0xFE20 && cp <= 0xFE2F) {
    return true;
  }
  return false;
}

int count_non_combining(std::string_view s) noexcept {
  int n = 0;
  for (std::size_t i = 0; i < s.size();) {
    char32_t cp = 0;
    i += utf8_next(s, i, cp);
    if (!is_combining(cp)) {
      ++n;
    }
  }
  return n;
}

}  // namespace

std::string truncate_chars(std::string_view text, int length,
                           std::string_view truncate_suffix) {
  if (length <= 0) {
    return {};
  }
  // calculate_truncate_chars_length
  int truncate_len = length;
  const int suf_len = count_non_combining(truncate_suffix);
  // add_truncation_text("", suffix) — if suffix has %(truncated_text)s, Python
  // handles it; for plain ellipsis suffix just subtract suffix non-combining.
  // When suffix is "…" (1 char), truncate_len = length - 1.
  // Match: for char in add_truncation_text("", replacement): if not combining: -=1
  // add_truncation_text("", "…") → "…" if no placeholder.
  if (truncate_suffix.find("%(truncated_text)s") == std::string_view::npos) {
    for (int i = 0; i < suf_len && truncate_len > 0; ++i) {
      --truncate_len;
    }
  }

  int s_len = 0;
  std::optional<std::size_t> end_index;
  for (std::size_t i = 0; i < text.size();) {
    const std::size_t before = i;
    char32_t cp = 0;
    i += utf8_next(text, i, cp);
    if (is_combining(cp)) {
      continue;
    }
    ++s_len;
    if (!end_index.has_value() && s_len > truncate_len) {
      end_index = before;
    }
    if (s_len > length) {
      std::string head(text.substr(0, end_index.value_or(0)));
      // append suffix if not already ending with it
      if (truncate_suffix.find("%(truncated_text)s") != std::string_view::npos) {
        // not handled here — caller should pass resolved suffix
        return head + std::string(truncate_suffix);
      }
      if (head.size() >= truncate_suffix.size() &&
          head.compare(head.size() - truncate_suffix.size(), truncate_suffix.size(),
                       truncate_suffix) == 0) {
        return head;
      }
      return head + std::string(truncate_suffix);
    }
  }
  return std::string(text);
}

std::string truncate_words(std::string_view text, int length,
                           std::string_view truncate_suffix) {
  if (length <= 0) {
    return {};
  }
  // split() — whitespace runs
  std::vector<std::string_view> words;
  std::size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() &&
           (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r' ||
            text[i] == '\f' || text[i] == '\v')) {
      ++i;
    }
    if (i >= text.size()) {
      break;
    }
    std::size_t start = i;
    while (i < text.size() &&
           !(text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r' ||
             text[i] == '\f' || text[i] == '\v')) {
      ++i;
    }
    words.emplace_back(text.substr(start, i - start));
  }
  if (static_cast<int>(words.size()) <= length) {
    std::string out;
    for (std::size_t w = 0; w < words.size(); ++w) {
      if (w) {
        out += ' ';
      }
      out += words[w];
    }
    return out;
  }
  std::string head;
  for (int w = 0; w < length; ++w) {
    if (w) {
      head += ' ';
    }
    head += words[static_cast<std::size_t>(w)];
  }
  if (truncate_suffix.find("%(truncated_text)s") != std::string_view::npos) {
    return head + std::string(truncate_suffix);
  }
  if (head.size() >= truncate_suffix.size() &&
      head.compare(head.size() - truncate_suffix.size(), truncate_suffix.size(),
                   truncate_suffix) == 0) {
    return head;
  }
  return head + std::string(truncate_suffix);
}

std::string querydict_urlencode(
    const std::vector<std::pair<std::string, std::string>>& pairs,
    std::string_view safe) {
  // quote each key/value with given safe, join as k=v&k=v
  // Uses filters.url_quote (UTF-8 percent-encoding).
  std::string out;
  for (std::size_t i = 0; i < pairs.size(); ++i) {
    if (i) {
      out += '&';
    }
    out += url_quote(pairs[i].first, safe);
    out += '=';
    out += url_quote(pairs[i].second, safe);
  }
  return out;
}

}  // namespace django::native
