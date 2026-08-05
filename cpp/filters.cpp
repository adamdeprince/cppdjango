#include "filters.hpp"
#include "html.hpp"

#include <cctype>
#include <climits>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace django::native {
namespace {

[[nodiscard]] int hex_nibble(unsigned v) noexcept {
  return static_cast<int>(v < 10 ? '0' + v : 'A' + (v - 10));
}

[[nodiscard]] bool is_unreserved_or_safe(unsigned char c,
                                         std::string_view safe) noexcept {
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
      c == '-' || c == '.' || c == '_' || c == '~') {
    return true;
  }
  return safe.find(static_cast<char>(c)) != std::string_view::npos;
}

}  // namespace

std::string filter_addslashes(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (char c : value) {
    if (c == '\\' || c == '"' || c == '\'') {
      out += '\\';
    }
    out += c;
  }
  return out;
}

std::string filter_capfirst(std::string_view value) {
  if (value.empty()) {
    return {};
  }
  std::string out(value);
  // ASCII-only first-char upper (Unicode left to Python when needed).
  unsigned char c0 = static_cast<unsigned char>(out[0]);
  if (c0 >= 'a' && c0 <= 'z') {
    out[0] = static_cast<char>(c0 - 'a' + 'A');
  }
  return out;
}

std::string filter_lower(std::string_view value) {
  std::string out(value);
  for (char& c : out) {
    unsigned char uc = static_cast<unsigned char>(c);
    if (uc >= 'A' && uc <= 'Z') {
      c = static_cast<char>(uc - 'A' + 'a');
    }
  }
  return out;
}

std::string filter_upper(std::string_view value) {
  std::string out(value);
  for (char& c : out) {
    unsigned char uc = static_cast<unsigned char>(c);
    if (uc >= 'a' && uc <= 'z') {
      c = static_cast<char>(uc - 'a' + 'A');
    }
  }
  return out;
}

std::string filter_cut(std::string_view value, std::string_view arg) {
  if (arg.empty()) {
    return std::string(value);
  }
  std::string out;
  out.reserve(value.size());
  std::size_t i = 0;
  while (i < value.size()) {
    if (i + arg.size() <= value.size() && value.substr(i, arg.size()) == arg) {
      i += arg.size();
    } else {
      out += value[i++];
    }
  }
  return out;
}

std::size_t filter_wordcount(std::string_view value) {
  // Approximate str.split(): runs of whitespace as separators.
  std::size_t count = 0;
  bool in_word = false;
  for (unsigned char c : value) {
    const bool ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
                     c == '\v' || c == 0xA0);
    if (ws) {
      in_word = false;
    } else if (!in_word) {
      in_word = true;
      ++count;
    }
  }
  return count;
}

std::string filter_ljust(std::string_view value, int width) {
  if (width <= static_cast<int>(value.size())) {
    return std::string(value);
  }
  std::string out(value);
  out.append(static_cast<std::size_t>(width) - value.size(), ' ');
  return out;
}

std::string filter_rjust(std::string_view value, int width) {
  if (width <= static_cast<int>(value.size())) {
    return std::string(value);
  }
  std::string out(static_cast<std::size_t>(width) - value.size(), ' ');
  out.append(value);
  return out;
}

std::string filter_center(std::string_view value, int width) {
  if (width <= 0 || width <= static_cast<int>(value.size())) {
    return std::string(value);
  }
  const std::size_t pad = static_cast<std::size_t>(width) - value.size();
  const std::size_t left = pad / 2;
  const std::size_t right = pad - left;
  std::string out;
  out.reserve(static_cast<std::size_t>(width));
  out.append(left, ' ');
  out.append(value);
  out.append(right, ' ');
  return out;
}

std::string filter_title(std::string_view value) {
  // Approximate str.title() for ASCII, then Django post-process:
  //   re.sub("([a-z])'([A-Z])", lower), re.sub(r"\d([A-Z])", lower)
  std::string out(value);
  bool cap_next = true;
  for (char& c : out) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if ((uc >= 'a' && uc <= 'z') || (uc >= 'A' && uc <= 'Z')) {
      if (cap_next) {
        if (uc >= 'a' && uc <= 'z') {
          c = static_cast<char>(uc - 'a' + 'A');
        }
        cap_next = false;
      } else {
        if (uc >= 'A' && uc <= 'Z') {
          c = static_cast<char>(uc - 'A' + 'a');
        }
      }
    } else if (uc >= '0' && uc <= '9') {
      // Digits are word chars in Python title — don't reset, keep lower after.
      cap_next = false;
    } else {
      // Apostrophe is a word char in Python title (don't capitalize after ').
      if (c == '\'') {
        cap_next = false;
      } else {
        cap_next = true;
      }
    }
  }
  // Django: re.sub("([a-z])'([A-Z])", lambda m: m[0].lower(), ...)
  for (std::size_t i = 0; i + 2 < out.size(); ++i) {
    if (out[i] >= 'a' && out[i] <= 'z' && out[i + 1] == '\'' &&
        out[i + 2] >= 'A' && out[i + 2] <= 'Z') {
      out[i + 2] = static_cast<char>(out[i + 2] - 'A' + 'a');
    }
  }
  // Django: re.sub(r"\d([A-Z])", lambda m: m[0].lower(), ...)
  for (std::size_t i = 0; i + 1 < out.size(); ++i) {
    if (out[i] >= '0' && out[i] <= '9' && out[i + 1] >= 'A' &&
        out[i + 1] <= 'Z') {
      out[i + 1] = static_cast<char>(out[i + 1] - 'A' + 'a');
    }
  }
  return out;
}

std::optional<std::string> filter_slice_string(std::string_view value,
                                               std::optional<int> start,
                                               std::optional<int> stop,
                                               std::optional<int> step) {
  const int n = static_cast<int>(value.size());
  int st = step.value_or(1);
  if (st == 0) {
    return std::nullopt;
  }

  auto resolve = [n](std::optional<int> idx, int default_when_none) {
    if (!idx.has_value()) {
      return default_when_none;
    }
    int i = *idx;
    if (i < 0) {
      i += n;
      if (i < 0) {
        i = 0;
      }
    }
    if (i > n) {
      i = n;
    }
    return i;
  };

  // Python slice with step:
  // positive step: start default 0, stop default n
  // negative step: start default n-1, stop default -n-1 (before 0)
  std::string out;
  if (st > 0) {
    int i = resolve(start, 0);
    int j = resolve(stop, n);
    for (; i < j; i += st) {
      out += value[static_cast<std::size_t>(i)];
    }
  } else {
    // Negative step: defaults differ.
    int i;
    if (start.has_value()) {
      int s = *start;
      if (s < 0) {
        s += n;
      }
      if (s < 0) {
        s = -1;  // clamp below
      }
      if (s >= n) {
        s = n - 1;
      }
      i = s;
    } else {
      i = n - 1;
    }
    int j;
    if (stop.has_value()) {
      int s = *stop;
      if (s < 0) {
        s += n;
      }
      // stop is exclusive; for negative step, walk while i > j
      if (s < -1) {
        s = -1;
      }
      if (s >= n) {
        s = n;
      }
      j = s;
    } else {
      j = -1;
    }
    for (; i > j; i += st) {
      if (i >= 0 && i < n) {
        out += value[static_cast<std::size_t>(i)];
      }
    }
  }
  return out;
}

std::string url_quote(std::string_view value, std::string_view safe) {
  std::string out;
  out.reserve(value.size() * 3);
  for (unsigned char c : value) {
    if (is_unreserved_or_safe(c, safe)) {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += static_cast<char>(hex_nibble(c >> 4));
      out += static_cast<char>(hex_nibble(c & 0xF));
    }
  }
  return out;
}

std::string escape_leading_slashes(std::string_view url) {
  if (url.size() >= 2 && url[0] == '/' && url[1] == '/') {
    std::string out = "/%2F";
    out.append(url.substr(2));
    return out;
  }
  return std::string(url);
}

std::string phone2numeric(std::string_view phone) {
  std::string out;
  out.reserve(phone.size());
  for (unsigned char c : phone) {
    char lc = static_cast<char>(c);
    if (c >= 'A' && c <= 'Z') {
      lc = static_cast<char>(c - 'A' + 'a');
    }
    char mapped = lc;
    switch (lc) {
      case 'a':
      case 'b':
      case 'c':
        mapped = '2';
        break;
      case 'd':
      case 'e':
      case 'f':
        mapped = '3';
        break;
      case 'g':
      case 'h':
      case 'i':
        mapped = '4';
        break;
      case 'j':
      case 'k':
      case 'l':
        mapped = '5';
        break;
      case 'm':
      case 'n':
      case 'o':
        mapped = '6';
        break;
      case 'p':
      case 'q':
      case 'r':
      case 's':
        mapped = '7';
        break;
      case 't':
      case 'u':
      case 'v':
        mapped = '8';
        break;
      case 'w':
      case 'x':
      case 'y':
      case 'z':
        mapped = '9';
        break;
      default:
        mapped = static_cast<char>(c);
        break;
    }
    out += mapped;
  }
  return out;
}

std::string normalize_newlines(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r') {
      if (i + 1 < text.size() && text[i + 1] == '\n') {
        ++i;
      }
      out += '\n';
    } else {
      out += text[i];
    }
  }
  return out;
}

std::string strip_spaces_between_tags(std::string_view value) {
  // re.sub(r">\s+<", "><", value)
  std::string out;
  out.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '>') {
      std::size_t j = i + 1;
      while (j < value.size() &&
             (value[j] == ' ' || value[j] == '\t' || value[j] == '\n' ||
              value[j] == '\r' || value[j] == '\f' || value[j] == '\v')) {
        ++j;
      }
      if (j < value.size() && value[j] == '<' && j > i + 1) {
        out += "><";
        i = j;  // loop ++ makes it j+1... set i = j so ++ lands on after '<'
        // Wait: we want to emit >< and continue after '<'. So i should be j
        // (the '<'), then for-loop ++i moves past it — wrong, we'd skip '<'.
        // Emit >< and set i = j (position of <), then continue will ++i past <.
        // Actually we need to include '<' in output: "><" already has both.
        // So set i = j (at '<'), then ++i → after '<'. Good if we emitted "><".
        continue;
      }
    }
    out += value[i];
  }
  return out;
}

std::string camel_case_to_spaces(std::string_view value) {
  // Django: re_camel_case = r"(((?<=[a-z])[A-Z])|([A-Z](?![A-Z]|$)))"
  std::string tmp;
  tmp.reserve(value.size() + 8);
  for (std::size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    const bool is_upper = c >= 'A' && c <= 'Z';
    if (is_upper) {
      const bool after_lower = i > 0 && value[i - 1] >= 'a' && value[i - 1] <= 'z';
      // [A-Z](?![A-Z]|$) — not at end and not followed by uppercase
      const bool capital_before_non_upper =
          (i + 1 < value.size()) && !(value[i + 1] >= 'A' && value[i + 1] <= 'Z');
      if (after_lower || capital_before_non_upper) {
        tmp += ' ';
      }
    }
    tmp += c;
  }
  std::size_t a = 0, b = tmp.size();
  while (a < b && (tmp[a] == ' ' || tmp[a] == '\t')) {
    ++a;
  }
  while (b > a && (tmp[b - 1] == ' ' || tmp[b - 1] == '\t')) {
    --b;
  }
  std::string out = tmp.substr(a, b - a);
  for (char& ch : out) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return out;
}

std::string pluralize_suffix(bool singular, std::string_view arg) {
  std::string a(arg);
  if (a.find(',') == std::string::npos) {
    a = std::string(",") + a;
  }
  const std::size_t comma = a.find(',');
  std::string singular_suffix = a.substr(0, comma);
  std::string plural_suffix = a.substr(comma + 1);
  // only one comma allowed
  if (plural_suffix.find(',') != std::string::npos) {
    return "";
  }
  return singular ? singular_suffix : plural_suffix;
}

std::string yesno(int tri_state, std::string_view arg) {
  // tri_state: 1 true, 0 false, -1 none
  std::vector<std::string> bits;
  std::size_t start = 0;
  while (start <= arg.size()) {
    auto c = arg.find(',', start);
    if (c == std::string_view::npos) {
      bits.emplace_back(arg.substr(start));
      break;
    }
    bits.emplace_back(arg.substr(start, c - start));
    start = c + 1;
  }
  if (bits.size() < 2) {
    return "";  // signal invalid — Python returns value
  }
  std::string yes = bits[0];
  std::string no = bits[1];
  std::string maybe = bits.size() >= 3 ? bits[2] : bits[1];
  if (tri_state < 0) {
    return maybe;
  }
  return tri_state ? yes : no;
}

int get_digit(std::int64_t value, int arg) {
  if (arg < 1) {
    return static_cast<int>(value);  // Python returns original value — handle in binding
  }
  if (value < 0) {
    value = -value;
  }
  std::string s = std::to_string(value);
  if (static_cast<std::size_t>(arg) > s.size()) {
    return 0;
  }
  return s[s.size() - static_cast<std::size_t>(arg)] - '0';
}

std::string widthratio(double value, double max_value, int max_width) {
  if (max_value == 0.0) {
    return "0";
  }
  const double ratio = (value / max_value) * static_cast<double>(max_width);
  // Python round(inf/nan) raises OverflowError → template returns "".
  if (!std::isfinite(ratio)) {
    throw std::overflow_error("widthratio overflow");
  }
  // Python 3 round uses banker's rounding (half to even).
  const double rounded = std::nearbyint(ratio);
  if (!std::isfinite(rounded) ||
      rounded > static_cast<double>(LLONG_MAX) ||
      rounded < static_cast<double>(LLONG_MIN)) {
    throw std::overflow_error("widthratio overflow");
  }
  return std::to_string(static_cast<long long>(rounded));
}

std::pair<std::string, std::string> get_mod_func(std::string_view callback) {
  const auto dot = callback.rfind('.');
  if (dot == std::string_view::npos) {
    return {std::string(callback), ""};
  }
  return {std::string(callback.substr(0, dot)),
          std::string(callback.substr(dot + 1))};
}

std::string iri_to_uri(std::string_view iri) {
  // quote(iri, safe="/#%[]=:;$&()+,!?*@'~")
  return url_quote(iri, "/#%[]=:;$&()+,!?*@'~");
}

namespace {

[[nodiscard]] int from_hex(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return -1;
}

// Django uri_to_iri only unquotes unreserved ASCII and bytes >= 0x80.
[[nodiscard]] bool uri_to_iri_should_unquote(unsigned char b) noexcept {
  if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') ||
      b == '-' || b == '.' || b == '_' || b == '~') {
    return true;
  }
  return b >= 0x80;
}

// repercent_broken_unicode: re-percent-encode illegal UTF-8 octets.
std::string repercent_broken_unicode(std::string bytes) {
  constexpr std::string_view kSafe = "/#%[]=:;$&()+,!?*@'~";
  std::string out;
  out.reserve(bytes.size());
  std::size_t i = 0;
  while (i < bytes.size()) {
    const auto lead = static_cast<unsigned char>(bytes[i]);
    std::size_t need = 0;
    if (lead < 0x80) {
      out += static_cast<char>(lead);
      ++i;
      continue;
    } else if ((lead & 0xE0) == 0xC0) {
      need = 2;
    } else if ((lead & 0xF0) == 0xE0) {
      need = 3;
    } else if ((lead & 0xF8) == 0xF0) {
      need = 4;
    } else {
      // Invalid lead — repercent this byte.
      out += url_quote(std::string_view(bytes.data() + i, 1), kSafe);
      ++i;
      continue;
    }
    if (i + need > bytes.size()) {
      out += url_quote(std::string_view(bytes.data() + i, bytes.size() - i), kSafe);
      break;
    }
    bool ok = true;
    for (std::size_t k = 1; k < need; ++k) {
      if ((static_cast<unsigned char>(bytes[i + k]) & 0xC0) != 0x80) {
        ok = false;
        break;
      }
    }
    // Reject overlong / out-of-range roughly like UTF-8 decoder.
    if (ok) {
      char32_t cp = 0;
      if (need == 2) {
        cp = (static_cast<char32_t>(lead & 0x1F) << 6) |
             (static_cast<unsigned char>(bytes[i + 1]) & 0x3F);
        if (cp < 0x80) {
          ok = false;
        }
      } else if (need == 3) {
        cp = (static_cast<char32_t>(lead & 0x0F) << 12) |
             ((static_cast<unsigned char>(bytes[i + 1]) & 0x3F) << 6) |
             (static_cast<unsigned char>(bytes[i + 2]) & 0x3F);
        if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) {
          ok = false;
        }
      } else {
        cp = (static_cast<char32_t>(lead & 0x07) << 18) |
             ((static_cast<unsigned char>(bytes[i + 1]) & 0x3F) << 12) |
             ((static_cast<unsigned char>(bytes[i + 2]) & 0x3F) << 6) |
             (static_cast<unsigned char>(bytes[i + 3]) & 0x3F);
        if (cp < 0x10000 || cp > 0x10FFFF) {
          ok = false;
        }
      }
    }
    if (ok) {
      out.append(bytes.data() + i, need);
      i += need;
    } else {
      // Match Python UnicodeDecodeError span: repercent the bad prefix.
      // Python repercents e.start:e.end for one error, then continues.
      // Approximate: repercent one lead byte and continue (common case).
      out += url_quote(std::string_view(bytes.data() + i, 1), kSafe);
      ++i;
    }
  }
  return out;
}

}  // namespace

std::string uri_to_iri(std::string_view uri) {
  // Selective unquote on UTF-8 byte string, then repercent broken unicode.
  std::string raw;
  raw.reserve(uri.size());
  for (std::size_t i = 0; i < uri.size(); ++i) {
    if (uri[i] == '%' && i + 2 < uri.size()) {
      const int hi = from_hex(uri[i + 1]);
      const int lo = from_hex(uri[i + 2]);
      if (hi >= 0 && lo >= 0) {
        const unsigned char b = static_cast<unsigned char>((hi << 4) | lo);
        if (uri_to_iri_should_unquote(b)) {
          raw += static_cast<char>(b);
          i += 2;
          continue;
        }
      }
    }
    raw += uri[i];
  }
  return repercent_broken_unicode(std::move(raw));
}

std::string escape_uri_path(std::string_view path) {
  // quote(path, safe="/:@&+$,-_.!~*'()")
  return url_quote(path, "/:@&+$,-_.!~*'()");
}

std::string filepath_to_uri(std::string_view path) {
  // quote(str(path).replace("\\", "/"), safe="/~!*()'")
  std::string normalized;
  normalized.reserve(path.size());
  for (char c : path) {
    normalized += (c == '\\') ? '/' : c;
  }
  return url_quote(normalized, "/~!*()'");
}

bool divisibleby(std::int64_t value, std::int64_t arg) {
  if (arg == 0) {
    // Python raises ZeroDivisionError; signal via throw.
    throw std::invalid_argument("division by zero");
  }
  return value % arg == 0;
}

std::optional<std::int64_t> filter_add_int(std::int64_t value, std::int64_t arg) {
  // Checked add.
  if (arg > 0) {
    if (value > std::numeric_limits<std::int64_t>::max() - arg) {
      return std::nullopt;
    }
  } else if (arg < 0) {
    if (value < std::numeric_limits<std::int64_t>::min() - arg) {
      return std::nullopt;
    }
  }
  return value + arg;
}

namespace {

// Decode one UTF-8 sequence; on invalid, consume 1 byte as U+FFFD-sized unit.
// Returns number of bytes consumed; writes [start, start+len) span.
std::size_t utf8_char_len(std::string_view s, std::size_t i) noexcept {
  if (i >= s.size()) {
    return 0;
  }
  const auto lead = static_cast<unsigned char>(s[i]);
  if (lead < 0x80) {
    return 1;
  }
  std::size_t need = 0;
  if ((lead & 0xE0) == 0xC0) {
    need = 2;
  } else if ((lead & 0xF0) == 0xE0) {
    need = 3;
  } else if ((lead & 0xF8) == 0xF0) {
    need = 4;
  } else {
    return 1;
  }
  if (i + need > s.size()) {
    return 1;
  }
  for (std::size_t k = 1; k < need; ++k) {
    if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
      return 1;
    }
  }
  return need;
}

// Word-wrap one line (TextWrapper with break_long_words=False,
// break_on_hyphens=False, replace_whitespace=False).
void wrap_line(std::string_view line, int width, std::vector<std::string>& out) {
  if (width <= 0) {
    out.emplace_back(line);
    return;
  }
  // If the line is only whitespace (or empty after wrap would drop it),
  // caller restores the original — we still produce chunks.
  std::size_t i = 0;
  const std::size_t n = line.size();
  std::string current;
  while (i < n) {
    // Skip leading spaces only at the start of a wrapped chunk when current empty
    // — replace_whitespace=False means we keep spaces as part of tokens carefully.
    // TextWrapper splits on whitespace; spaces between words become break points.
    // Leading whitespace on the original line is preserved on first chunk.
    if (current.empty() && i == 0) {
      // Capture leading whitespace into current (indent preserve).
      while (i < n && (line[i] == ' ' || line[i] == '\t')) {
        current += line[i++];
      }
    }
    // Find next word (non-whitespace run).
    while (i < n && (line[i] == ' ' || line[i] == '\t')) {
      // Whitespace between words: if current empty mid-line, skip? 
      // TextWrapper with replace_whitespace=False keeps spaces attached.
      // Actually spaces separate chunks; the break consumes the space.
      if (!current.empty()) {
        // Prefer breaking at this space if adding next word would exceed.
        // Look ahead to next word length.
        std::size_t j = i;
        while (j < n && (line[j] == ' ' || line[j] == '\t')) {
          ++j;
        }
        std::size_t k = j;
        while (k < n && line[k] != ' ' && line[k] != '\t') {
          ++k;
        }
        const std::size_t word_len = k - j;
        const std::size_t space_len = j - i;
        // If current + spaces + word > width and current non-empty, break.
        if (static_cast<int>(current.size() + space_len + word_len) > width &&
            !current.empty()) {
          out.push_back(current);
          current.clear();
          // Drop the separating spaces (they are the break point).
          i = j;
          continue;
        }
        // Append spaces then continue to word.
        current.append(line.substr(i, space_len));
        i = j;
        continue;
      }
      ++i;
    }
    if (i >= n) {
      break;
    }
    // Word
    std::size_t k = i;
    while (k < n && line[k] != ' ' && line[k] != '\t') {
      ++k;
    }
    const std::string_view word = line.substr(i, k - i);
    if (current.empty()) {
      // Long word: don't break (break_long_words=False).
      current.assign(word);
    } else if (static_cast<int>(current.size() + word.size()) <= width) {
      current.append(word);
    } else {
      out.push_back(current);
      current.assign(word);
    }
    i = k;
  }
  if (!current.empty()) {
    out.push_back(current);
  }
}

}  // namespace

std::size_t utf8_length(std::string_view value) {
  std::size_t count = 0;
  for (std::size_t i = 0; i < value.size();) {
    i += utf8_char_len(value, i);
    ++count;
  }
  return count;
}

std::string utf8_first(std::string_view value) {
  if (value.empty()) {
    return {};
  }
  return std::string(value.substr(0, utf8_char_len(value, 0)));
}

std::string utf8_last(std::string_view value) {
  if (value.empty()) {
    return {};
  }
  std::size_t last = 0;
  for (std::size_t i = 0; i < value.size();) {
    last = i;
    i += utf8_char_len(value, i);
  }
  return std::string(value.substr(last));
}

std::vector<std::string> make_list_chars(std::string_view value) {
  std::vector<std::string> out;
  out.reserve(value.size());
  for (std::size_t i = 0; i < value.size();) {
    const std::size_t len = utf8_char_len(value, i);
    out.emplace_back(value.substr(i, len));
    i += len;
  }
  return out;
}

std::string linenumbers(std::string_view value, bool autoescape) {
  // Split on \n keeping empty trailing segments like str.split("\n").
  std::vector<std::string_view> lines;
  std::size_t start = 0;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '\n') {
      lines.emplace_back(value.data() + start, i - start);
      start = i + 1;
    }
  }
  lines.emplace_back(value.data() + start, value.size() - start);

  const std::size_t nlines = lines.size();
  // width = len(str(nlines))
  std::size_t width = 1;
  for (std::size_t x = nlines; x >= 10; x /= 10) {
    ++width;
  }

  std::string out;
  out.reserve(value.size() + nlines * (width + 2));
  for (std::size_t i = 0; i < nlines; ++i) {
    if (i > 0) {
      out += '\n';
    }
    // Zero-pad line number.
    std::string num = std::to_string(i + 1);
    if (num.size() < width) {
      out.append(width - num.size(), '0');
    }
    out += num;
    out += ". ";
    if (autoescape) {
      // Avoid circular include: call html_escape via declaration in html.hpp.
      out += html_escape(lines[i]);
    } else {
      out += lines[i];
    }
  }
  return out;
}

std::string join_strings(const std::vector<std::string>& parts,
                         std::string_view sep) {
  if (parts.empty()) {
    return {};
  }
  std::size_t total = sep.size() * (parts.size() - 1);
  for (const auto& p : parts) {
    total += p.size();
  }
  std::string out;
  out.reserve(total);
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) {
      out += sep;
    }
    out += parts[i];
  }
  return out;
}

std::string wordwrap(std::string_view text, int width) {
  // Match django.utils.text.wrap: splitlines, wrap each, preserve trailing \n.
  std::vector<std::string> result;
  std::size_t start = 0;
  const bool ends_nl =
      !text.empty() && (text.back() == '\n' ||
                        (text.size() >= 2 && text[text.size() - 2] == '\r' &&
                         text.back() == '\n'));
  // Use splitlines-like: split on \n, \r\n, \r without keeping ends.
  for (std::size_t i = 0; i < text.size();) {
    if (text[i] == '\n') {
      std::string_view line(text.data() + start, i - start);
      std::vector<std::string> wrapped;
      wrap_line(line, width, wrapped);
      if (wrapped.empty()) {
        result.emplace_back(line);
      } else {
        for (auto& w : wrapped) {
          result.push_back(std::move(w));
        }
      }
      start = i + 1;
      ++i;
    } else if (text[i] == '\r') {
      std::string_view line(text.data() + start, i - start);
      std::vector<std::string> wrapped;
      wrap_line(line, width, wrapped);
      if (wrapped.empty()) {
        result.emplace_back(line);
      } else {
        for (auto& w : wrapped) {
          result.push_back(std::move(w));
        }
      }
      if (i + 1 < text.size() && text[i + 1] == '\n') {
        i += 2;
      } else {
        ++i;
      }
      start = i;
    } else {
      ++i;
    }
  }
  // Last line (if not ending mid-break already consumed).
  if (start < text.size() ||
      (!text.empty() && (text.back() == '\n' || text.back() == '\r'))) {
    // If text ends with newline, the final empty segment is handled by ends_nl.
    if (start <= text.size()) {
      std::string_view line(text.data() + start, text.size() - start);
      // When text ends with \n, start==size and line is empty — skip here;
      // ends_nl appends "".
      if (!(start == text.size() && ends_nl)) {
        std::vector<std::string> wrapped;
        wrap_line(line, width, wrapped);
        if (wrapped.empty()) {
          result.emplace_back(line);
        } else {
          for (auto& w : wrapped) {
            result.push_back(std::move(w));
          }
        }
      }
    }
  }
  // Preserve trailing newline.
  if (!text.empty() && text.back() == '\n') {
    result.emplace_back("");
  }

  std::string out;
  for (std::size_t i = 0; i < result.size(); ++i) {
    if (i > 0) {
      out += '\n';
    }
    out += result[i];
  }
  return out;
}

}  // namespace django::native
