#include "html.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace django::native {
namespace {

// Extra bytes beyond the original character for each HTML special.
// &amp; = 5 → +4, &lt;/&gt; = 4 → +3, &quot; = 6 → +5, &#x27; = 5 → +4
[[nodiscard]] constexpr std::size_t html_extra(char c) noexcept {
  switch (c) {
    case '&':
      return 4;
    case '<':
    case '>':
      return 3;
    case '"':
      return 5;
    case '\'':
      return 4;
    default:
      return 0;
  }
}

void append_html_escaped(std::string& out, char c) {
  switch (c) {
    case '&':
      out += "&amp;";
      break;
    case '<':
      out += "&lt;";
      break;
    case '>':
      out += "&gt;";
      break;
    case '"':
      out += "&quot;";
      break;
    case '\'':
      out += "&#x27;";
      break;
    default:
      out += c;
      break;
  }
}

// Decode one UTF-8 code point. On invalid sequences, emit U+FFFD and
// consume one byte so we always make progress.
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

// Hex nibble for \u00XX style escapes.
[[nodiscard]] constexpr char hex_digit(unsigned v) noexcept {
  return static_cast<char>(v < 10 ? '0' + v : 'A' + (v - 10));
}

void append_js_unicode_escape(std::string& out, char32_t cp) {
  // Always 4 hex digits (BMP); U+2028/U+2029 and C0 controls fit.
  out += "\\u";
  out += hex_digit((cp >> 12) & 0xF);
  out += hex_digit((cp >> 8) & 0xF);
  out += hex_digit((cp >> 4) & 0xF);
  out += hex_digit(cp & 0xF);
}

// Returns true and writes the JS escape when `cp` must be escaped.
[[nodiscard]] bool try_js_escape(std::string& out, char32_t cp) {
  if (cp < 32) {
    append_js_unicode_escape(out, cp);
    return true;
  }
  switch (cp) {
    case '\\':
      out += "\\u005C";
      return true;
    case '\'':
      out += "\\u0027";
      return true;
    case '"':
      out += "\\u0022";
      return true;
    case '>':
      out += "\\u003E";
      return true;
    case '<':
      out += "\\u003C";
      return true;
    case '&':
      out += "\\u0026";
      return true;
    case '=':
      out += "\\u003D";
      return true;
    case '-':
      out += "\\u002D";
      return true;
    case ';':
      out += "\\u003B";
      return true;
    case '`':
      out += "\\u0060";
      return true;
    case 0x2028:
      out += "\\u2028";
      return true;
    case 0x2029:
      out += "\\u2029";
      return true;
    default:
      return false;
  }
}

}  // namespace

std::string html_escape(std::string_view input) {
  std::size_t extra = 0;
  for (char c : input) {
    extra += html_extra(c);
  }
  if (extra == 0) {
    return std::string(input);
  }

  std::string out;
  out.reserve(input.size() + extra);
  for (char c : input) {
    if (html_extra(c) == 0) {
      out += c;
    } else {
      append_html_escaped(out, c);
    }
  }
  return out;
}

std::string escapejs(std::string_view input) {
  std::string out;
  // Worst case: every code unit becomes \uXXXX (6 chars).
  out.reserve(input.size() * 2);

  std::size_t i = 0;
  while (i < input.size()) {
    const char32_t cp = decode_utf8(input, i);
    if (!try_js_escape(out, cp)) {
      append_utf8(out, cp);
    }
  }
  return out;
}

namespace {

// Normalize \r\n / \r → \n (same as filters.normalize_newlines, local copy to
// avoid a hard link edge when only html is used).
std::string normalize_nl(std::string_view text) {
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

std::string replace_all(std::string_view s, std::string_view from,
                        std::string_view to) {
  if (from.empty()) {
    return std::string(s);
  }
  std::string out;
  out.reserve(s.size());
  std::size_t i = 0;
  while (i < s.size()) {
    if (i + from.size() <= s.size() && s.substr(i, from.size()) == from) {
      out += to;
      i += from.size();
    } else {
      out += s[i++];
    }
  }
  return out;
}

[[nodiscard]] bool is_name_start(unsigned char c) noexcept {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

[[nodiscard]] bool is_name_char(unsigned char c) noexcept {
  return is_name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == ':' ||
         c == '.' || c == '_';
}

// Result of attempting to skip a markup construct at '<'.
// end == npos and drop_to_eof false → not a tag, emit '<' as text.
// end == n → incomplete construct dropped through EOF (HTMLParser buffer discard).
// else end is index after the construct (skipped).
struct SkipResult {
  std::size_t end = std::string_view::npos;
  bool drop_to_eof = false;
};

SkipResult skip_tag_or_comment(std::string_view value, std::size_t i) {
  const std::size_t n = value.size();
  if (i >= n || value[i] != '<') {
    return {};
  }
  // Comment: <!-- ... -->  (unclosed: drop to EOF like HTMLParser close())
  if (i + 3 < n && value[i + 1] == '!' && value[i + 2] == '-' &&
      value[i + 3] == '-') {
    std::size_t j = i + 4;
    while (j + 2 < n) {
      if (value[j] == '-' && value[j + 1] == '-' && value[j + 2] == '>') {
        return {j + 3, false};
      }
      ++j;
    }
    // Unclosed comment — HTMLParser discards on close().
    return {n, true};
  }
  // Markup declaration / bogus comment: <!...>
  // Unclosed <!... (no '>') is treated as a comment to EOF (Py3.14+).
  if (i + 1 < n && value[i + 1] == '!') {
    std::size_t j = i + 2;
    while (j < n && value[j] != '>') {
      ++j;
    }
    if (j < n) {
      return {j + 1, false};
    }
    return {n, true};
  }
  // Processing instruction <? ... ?>
  if (i + 1 < n && value[i + 1] == '?') {
    std::size_t j = i + 2;
    while (j + 1 < n) {
      if (value[j] == '?' && value[j + 1] == '>') {
        return {j + 2, false};
      }
      ++j;
    }
    return {n, true};
  }
  // Closing or opening tag: </name ...> or <name ...>
  // HTMLParser is liberal: tag name can include odd chars until whitespace/'/'/'>'
  // for the start; we approximate by requiring a name start char.
  std::size_t j = i + 1;
  if (j < n && value[j] == '/') {
    ++j;
  }
  if (j >= n || !is_name_start(static_cast<unsigned char>(value[j]))) {
    // Not a tag (e.g. "<235" or "< f" or "<>").
    return {};
  }
  // Consume name-ish run (HTMLParser allows more than strict XML names).
  while (j < n) {
    const unsigned char c = static_cast<unsigned char>(value[j]);
    if (c == '/' || c == '>' || c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
        c == '\f') {
      break;
    }
    // Allow almost anything in the "name" until whitespace (matches HTMLParser
    // feeding odd tags like sc<!--).
    if (c == '"' || c == '\'') {
      break;
    }
    ++j;
  }
  // Scan to '>' respecting quotes.
  char quote = 0;
  for (; j < n; ++j) {
    const char c = value[j];
    if (quote) {
      if (c == quote) {
        quote = 0;
      }
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
      continue;
    }
    if (c == '>') {
      return {j + 1, false};
    }
  }
  // Unclosed tag — HTMLParser discards buffered markup on close().
  return {n, true};
}

// Emit entity/char ref starting at '&'. Always advances at least 1.
// Matches convert_charrefs=False + incomplete-entity semicolon completion.
void append_entity(std::string& out, std::string_view value, std::size_t& i) {
  const std::size_t n = value.size();
  // bare '&'
  if (i + 1 >= n) {
    out += '&';
    ++i;
    return;
  }
  if (value[i + 1] == '#') {
    // Char ref: &#digits; or &#xhex;
    std::size_t j = i + 2;
    bool hex = false;
    if (j < n && (value[j] == 'x' || value[j] == 'X')) {
      hex = true;
      ++j;
    }
    const std::size_t dig_start = j;
    if (hex) {
      while (j < n) {
        const unsigned char c = static_cast<unsigned char>(value[j]);
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F')) {
          ++j;
        } else {
          break;
        }
      }
    } else {
      while (j < n && value[j] >= '0' && value[j] <= '9') {
        ++j;
      }
    }
    if (j == dig_start) {
      // &# or &#x with no digits — leave as text through current char only.
      out += '&';
      ++i;
      return;
    }
    // Emit &#...; with forced semicolon (incomplete entity completion).
    out.append(value.substr(i, j - i));
    if (j >= n || value[j] != ';') {
      out += ';';
      i = j;
    } else {
      out += ';';
      i = j + 1;
    }
    return;
  }
  // Named entity: must start with a letter.
  if (!is_name_start(static_cast<unsigned char>(value[i + 1]))) {
    out += '&';
    ++i;
    return;
  }
  std::size_t j = i + 1;
  while (j < n) {
    const unsigned char c = static_cast<unsigned char>(value[j]);
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9')) {
      ++j;
    } else {
      break;
    }
  }
  out.append(value.substr(i, j - i));
  if (j >= n || value[j] != ';') {
    out += ';';
    i = j;
  } else {
    out += ';';
    i = j + 1;
  }
}

}  // namespace

std::string linebreaks(std::string_view value, bool autoescape) {
  const std::string norm = normalize_nl(value);
  // Split on \n{2,}
  std::vector<std::string_view> paras;
  std::size_t start = 0;
  for (std::size_t i = 0; i < norm.size();) {
    if (norm[i] == '\n') {
      std::size_t j = i;
      while (j < norm.size() && norm[j] == '\n') {
        ++j;
      }
      if (j - i >= 2) {
        paras.emplace_back(norm.data() + start, i - start);
        start = j;
        i = j;
        continue;
      }
    }
    ++i;
  }
  paras.emplace_back(norm.data() + start, norm.size() - start);

  std::string out;
  for (std::size_t p = 0; p < paras.size(); ++p) {
    if (p > 0) {
      out += "\n\n";
    }
    std::string body(paras[p]);
    if (autoescape) {
      body = html_escape(body);
    }
    body = replace_all(body, "\n", "<br>");
    out += "<p>";
    out += body;
    out += "</p>";
  }
  return out;
}

std::string linebreaksbr(std::string_view value, bool autoescape) {
  std::string body = normalize_nl(value);
  if (autoescape) {
    body = html_escape(body);
  }
  return replace_all(body, "\n", "<br>");
}

std::string strip_tags_once(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (std::size_t i = 0; i < value.size();) {
    if (value[i] == '<') {
      const SkipResult sk = skip_tag_or_comment(value, i);
      if (sk.end != std::string_view::npos) {
        // Complete tag/comment or incomplete markup discarded through EOF.
        i = sk.end;
        continue;
      }
      // Not recognized as markup — emit '<'.
      out += '<';
      ++i;
      continue;
    }
    if (value[i] == '&') {
      append_entity(out, value, i);
      continue;
    }
    out += value[i];
    ++i;
  }
  return out;
}

std::string strip_tags(std::string_view value) {
  constexpr int kMaxDepth = 50;
  // Django: long_open_tag_without_closing_re = r"<[a-zA-Z][^>]{1000,}"
  // if match.group().count("<") >= 50 → SuspiciousOperation.
  {
    const std::size_t n = value.size();
    for (std::size_t i = 0; i < n; ++i) {
      if (value[i] != '<' || i + 1 >= n ||
          !is_name_start(static_cast<unsigned char>(value[i + 1]))) {
        continue;
      }
      std::size_t j = i + 1;  // letter
      ++j;
      std::size_t non_gt = 0;
      while (j < n && value[j] != '>') {
        ++non_gt;
        ++j;
      }
      if (non_gt >= 1000) {
        const std::size_t match_end = j;  // at '>' or n
        int lt = 0;
        for (std::size_t k = i; k < match_end; ++k) {
          if (value[k] == '<') {
            ++lt;
          }
        }
        if (lt >= kMaxDepth) {
          throw std::runtime_error("SuspiciousOperation");
        }
      }
    }
  }

  std::string cur(value);
  int depth = 0;
  while (cur.find('<') != std::string::npos && cur.find('>') != std::string::npos) {
    if (depth >= kMaxDepth) {
      throw std::runtime_error("SuspiciousOperation");
    }
    std::string next = strip_tags_once(cur);
    const auto count_lt = [](const std::string& s) {
      return static_cast<int>(std::count(s.begin(), s.end(), '<'));
    };
    if (count_lt(cur) == count_lt(next)) {
      break;
    }
    cur = std::move(next);
    ++depth;
  }
  return cur;
}

namespace {

bool is_void_element(std::string_view tag) {
  static constexpr const char* kVoid[] = {
      "area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta",
      "param", "source", "track", "wbr", "frame", "spacer"};
  std::string lower;
  lower.reserve(tag.size());
  for (char c : tag) {
    lower += static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
  }
  for (const char* v : kVoid) {
    if (lower == v) {
      return true;
    }
  }
  return false;
}

// Count non-combining Unicode code points (suffix budget).
int count_display_chars(std::string_view s) {
  int n = 0;
  for (std::size_t i = 0; i < s.size();) {
    auto lead = static_cast<unsigned char>(s[i]);
    char32_t cp = 0;
    if (lead < 0x80) {
      cp = lead;
      ++i;
    } else if ((lead & 0xE0) == 0xC0 && i + 1 < s.size()) {
      cp = ((lead & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
      i += 2;
    } else if ((lead & 0xF0) == 0xE0 && i + 2 < s.size()) {
      cp = ((lead & 0x0F) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
           (static_cast<unsigned char>(s[i + 2]) & 0x3F);
      i += 3;
    } else if ((lead & 0xF8) == 0xF0 && i + 3 < s.size()) {
      cp = ((lead & 0x07) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
           ((static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) |
           (static_cast<unsigned char>(s[i + 3]) & 0x3F);
      i += 4;
    } else {
      ++i;
      continue;
    }
    if (!((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
          (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) ||
          (cp >= 0xFE20 && cp <= 0xFE2F))) {
      ++n;
    }
  }
  return n;
}

// Code-point count (Python len for UTF-8).
int utf8_len(std::string_view s) {
  int n = 0;
  for (std::size_t i = 0; i < s.size();) {
    auto lead = static_cast<unsigned char>(s[i]);
    if (lead < 0x80) {
      ++i;
    } else if ((lead & 0xE0) == 0xC0) {
      i += 2;
    } else if ((lead & 0xF0) == 0xE0) {
      i += 3;
    } else if ((lead & 0xF8) == 0xF0) {
      i += 4;
    } else {
      ++i;
    }
    ++n;
  }
  return n;
}

// First n Unicode code points.
std::string utf8_prefix(std::string_view s, int n) {
  if (n <= 0) {
    return {};
  }
  std::size_t i = 0;
  int cps = 0;
  while (i < s.size() && cps < n) {
    auto lead = static_cast<unsigned char>(s[i]);
    if (lead < 0x80) {
      ++i;
    } else if ((lead & 0xE0) == 0xC0) {
      i += 2;
    } else if ((lead & 0xF0) == 0xE0) {
      i += 3;
    } else if ((lead & 0xF8) == 0xF0) {
      i += 4;
    } else {
      ++i;
    }
    ++cps;
  }
  return std::string(s.substr(0, std::min(i, s.size())));
}

void append_utf8_cp(std::string& out, char32_t cp) {
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

// Minimal HTML entity decode (HTMLParser convert_charrefs=True subset).
// Returns decoded string. Unknown named entities are left as-is (chars).
std::string decode_charrefs(std::string_view data) {
  std::string out;
  out.reserve(data.size());
  for (std::size_t i = 0; i < data.size();) {
    if (data[i] != '&') {
      out += data[i];
      ++i;
      continue;
    }
    // Find end of entity ('; or whitespace/end)
    std::size_t j = i + 1;
    if (j >= data.size()) {
      out += '&';
      ++i;
      continue;
    }
    if (data[j] == '#') {
      ++j;
      bool hex = false;
      if (j < data.size() && (data[j] == 'x' || data[j] == 'X')) {
        hex = true;
        ++j;
      }
      std::size_t num_start = j;
      while (j < data.size()) {
        char c = data[j];
        if (hex) {
          if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                (c >= 'A' && c <= 'F'))) {
            break;
          }
        } else if (c < '0' || c > '9') {
          break;
        }
        ++j;
      }
      if (j > num_start) {
        unsigned long v = 0;
        auto part = data.substr(num_start, j - num_start);
        try {
          v = std::stoul(std::string(part), nullptr, hex ? 16 : 10);
        } catch (...) {
          out += data[i];
          ++i;
          continue;
        }
        if (j < data.size() && data[j] == ';') {
          ++j;
        }
        if (v == 0 || v > 0x10FFFF) {
          // HTML5 replaces some; keep replacement
          append_utf8_cp(out, 0xFFFD);
        } else {
          append_utf8_cp(out, static_cast<char32_t>(v));
        }
        i = j;
        continue;
      }
      out += '&';
      ++i;
      continue;
    }
    // Named entity
    std::size_t name_start = j;
    while (j < data.size() &&
           ((data[j] >= 'A' && data[j] <= 'Z') || (data[j] >= 'a' && data[j] <= 'z') ||
            (data[j] >= '0' && data[j] <= '9'))) {
      ++j;
    }
    if (j > name_start && j < data.size() && data[j] == ';') {
      std::string name(data.substr(name_start, j - name_start));
      // Common entities used by Django tests + HTML specials
      struct Ent {
        const char* n;
        char32_t cp;
      };
      static constexpr Ent kEnts[] = {
          {"amp", '&'},      {"lt", '<'},       {"gt", '>'},       {"quot", '"'},
          {"apos", '\''},    {"nbsp", 0xA0},    {"copy", 0xA9},    {"reg", 0xAE},
          {"deg", 0xB0},     {"plusmn", 0xB1},  {"sup2", 0xB2},    {"sup3", 0xB3},
          {"micro", 0xB5},   {"para", 0xB6},    {"middot", 0xB7},  {"sup1", 0xB9},
          {"frac14", 0xBC},  {"frac12", 0xBD},  {"frac34", 0xBE},  {"times", 0xD7},
          {"divide", 0xF7},  {"forall", 0x2200},{"part", 0x2202},  {"exist", 0x2203},
          {"empty", 0x2205}, {"nabla", 0x2207}, {"isin", 0x2208},  {"notin", 0x2209},
          {"ni", 0x220B},    {"prod", 0x220F},  {"sum", 0x2211},   {"minus", 0x2212},
          {"lowast", 0x2217},{"radic", 0x221A}, {"prop", 0x221D},  {"infin", 0x221E},
          {"ang", 0x2220},   {"and", 0x2227},   {"or", 0x2228},    {"cap", 0x2229},
          {"cup", 0x222A},   {"int", 0x222B},   {"there4", 0x2234},{"sim", 0x223C},
          {"cong", 0x2245},  {"asymp", 0x2248}, {"ne", 0x2260},    {"equiv", 0x2261},
          {"le", 0x2264},    {"ge", 0x2265},    {"sub", 0x2282},   {"sup", 0x2283},
          {"nsub", 0x2284},  {"sube", 0x2286},  {"supe", 0x2287},  {"oplus", 0x2295},
          {"otimes", 0x2297},{"perp", 0x22A5},  {"sdot", 0x22C5},  {"lceil", 0x2308},
          {"rceil", 0x2309}, {"lfloor", 0x230A},{"rfloor", 0x230B},{"lang", 0x2329},
          {"rang", 0x232A},  {"loz", 0x25CA},   {"spades", 0x2660},{"clubs", 0x2663},
          {"hearts", 0x2665},{"diams", 0x2666},
          // Latin-1 named (subset + accented vowels for tests)
          {"Agrave", 0xC0},  {"Aacute", 0xC1},  {"Acirc", 0xC2},   {"Atilde", 0xC3},
          {"Auml", 0xC4},    {"Aring", 0xC5},   {"AElig", 0xC6},   {"Ccedil", 0xC7},
          {"Egrave", 0xC8},  {"Eacute", 0xC9},  {"Ecirc", 0xCA},   {"Euml", 0xCB},
          {"Igrave", 0xCC},  {"Iacute", 0xCD},  {"Icirc", 0xCE},   {"Iuml", 0xCF},
          {"ETH", 0xD0},     {"Ntilde", 0xD1},  {"Ograve", 0xD2},  {"Oacute", 0xD3},
          {"Ocirc", 0xD4},   {"Otilde", 0xD5},  {"Ouml", 0xD6},    {"Oslash", 0xD8},
          {"Ugrave", 0xD9},  {"Uacute", 0xDA},  {"Ucirc", 0xDB},   {"Uuml", 0xDC},
          {"Yacute", 0xDD},  {"THORN", 0xDE},   {"szlig", 0xDF},
          {"agrave", 0xE0},  {"aacute", 0xE1},  {"acirc", 0xE2},   {"atilde", 0xE3},
          {"auml", 0xE4},    {"aring", 0xE5},   {"aelig", 0xE6},   {"ccedil", 0xE7},
          {"egrave", 0xE8},  {"eacute", 0xE9},  {"ecirc", 0xEA},   {"euml", 0xEB},
          {"igrave", 0xEC},  {"iacute", 0xED},  {"icirc", 0xEE},   {"iuml", 0xEF},
          {"eth", 0xF0},     {"ntilde", 0xF1},  {"ograve", 0xF2},  {"oacute", 0xF3},
          {"ocirc", 0xF4},   {"otilde", 0xF5},  {"ouml", 0xF6},    {"oslash", 0xF8},
          {"ugrave", 0xF9},  {"uacute", 0xFA},  {"ucirc", 0xFB},   {"uuml", 0xFC},
          {"yacute", 0xFD},  {"thorn", 0xFE},   {"yuml", 0xFF},
          {"OElig", 0x152},  {"oelig", 0x153},  {"Scaron", 0x160}, {"scaron", 0x161},
          {"Yuml", 0x178},   {"fnof", 0x192},   {"circ", 0x2C6},    {"tilde", 0x2DC},
          {"ensp", 0x2002},  {"emsp", 0x2003},  {"thinsp", 0x2009},{"zwnj", 0x200C},
          {"zwj", 0x200D},   {"lrm", 0x200E},   {"rlm", 0x200F},   {"ndash", 0x2013},
          {"mdash", 0x2014}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"sbquo", 0x201A},
          {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"bdquo", 0x201E}, {"dagger", 0x2020},
          {"Dagger", 0x2021},{"bull", 0x2022},  {"hellip", 0x2026},{"permil", 0x2030},
          {"prime", 0x2032}, {"Prime", 0x2033}, {"lsaquo", 0x2039},{"rsaquo", 0x203A},
          {"oline", 0x203E}, {"euro", 0x20AC},  {"trade", 0x2122}, {"larr", 0x2190},
          {"uarr", 0x2191},  {"rarr", 0x2192},  {"darr", 0x2193},  {"harr", 0x2194},
          {"crarr", 0x21B5}, {"lArr", 0x21D0},  {"uArr", 0x21D1},  {"rArr", 0x21D2},
          {"dArr", 0x21D3},  {"hArr", 0x21D4},  {"Alpha", 0x391},  {"Beta", 0x392},
          {"Gamma", 0x393},  {"Delta", 0x394},  {"Epsilon", 0x395},{"Zeta", 0x396},
          {"Eta", 0x397},    {"Theta", 0x398},  {"Iota", 0x399},   {"Kappa", 0x39A},
          {"Lambda", 0x39B}, {"Mu", 0x39C},     {"Nu", 0x39D},     {"Xi", 0x39E},
          {"Omicron", 0x39F},{"Pi", 0x3A0},     {"Rho", 0x3A1},    {"Sigma", 0x3A3},
          {"Tau", 0x3A4},    {"Upsilon", 0x3A5},{"Phi", 0x3A6},    {"Chi", 0x3A7},
          {"Psi", 0x3A8},    {"Omega", 0x3A9},  {"alpha", 0x3B1},  {"beta", 0x3B2},
          {"gamma", 0x3B3},  {"delta", 0x3B4},  {"epsilon", 0x3B5},{"zeta", 0x3B6},
          {"eta", 0x3B7},    {"theta", 0x3B8},  {"iota", 0x3B9},   {"kappa", 0x3BA},
          {"lambda", 0x3BB}, {"mu", 0x3BC},     {"nu", 0x3BD},     {"xi", 0x3BE},
          {"omicron", 0x3BF},{"pi", 0x3C0},     {"rho", 0x3C1},    {"sigmaf", 0x3C2},
          {"sigma", 0x3C3},  {"tau", 0x3C4},    {"upsilon", 0x3C5},{"phi", 0x3C6},
          {"chi", 0x3C7},    {"psi", 0x3C8},    {"omega", 0x3C9},  {"thetasym", 0x3D1},
          {"upsih", 0x3D2},  {"piv", 0x3D6},
      };
      bool found = false;
      for (const auto& e : kEnts) {
        if (name == e.n) {
          append_utf8_cp(out, e.cp);
          found = true;
          break;
        }
      }
      if (found) {
        i = j + 1;  // skip ';'
        continue;
      }
    }
    // Not a known entity — emit '&' literally and continue
    out += '&';
    ++i;
  }
  return out;
}

std::string extract_tag_name(std::string_view tag_text) {
  std::size_t i = 1;  // skip <
  if (i < tag_text.size() && tag_text[i] == '/') {
    ++i;
  }
  // comments / doctype — no name
  if (i < tag_text.size() && (tag_text[i] == '!' || tag_text[i] == '?')) {
    return {};
  }
  std::size_t start = i;
  while (i < tag_text.size()) {
    unsigned char c = static_cast<unsigned char>(tag_text[i]);
    if (c == '/' || c == '>' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      break;
    }
    ++i;
  }
  std::string name(tag_text.substr(start, i - start));
  for (char& ch : name) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return name;
}

bool is_end_tag(std::string_view tag_text) {
  return tag_text.size() >= 2 && tag_text[1] == '/';
}

bool is_self_closing(std::string_view tag_text) {
  if (tag_text.size() >= 2 && tag_text[tag_text.size() - 2] == '/') {
    return true;
  }
  return false;
}

// Core HTML truncate. by_words=false → chars; true → words.
// Matches TruncateCharsHTMLParser / TruncateWordsHTMLParser for well-formed markup.
std::string truncate_html_impl(std::string_view html, int length,
                               std::string_view suffix, bool by_words) {
  if (length <= 0) {
    return {};
  }
  // Budget: chars subtract non-combining suffix length; words use raw length.
  // TruncateCharsHTMLParser keeps original `length` on self.length and sets
  // remaining to calculate_truncate_chars_length (suffix-adjusted).
  const int original_length = length;
  int remaining = length;
  if (!by_words) {
    remaining -= count_display_chars(suffix);
    if (remaining < 0) {
      remaining = 0;
    }
  }

  std::string output;
  std::deque<std::string> tags;  // front = most recently opened (LIFO)
  int processed_chars = 0;       // only for chars path exact-fit edge case

  auto close_open = [&]() {
    for (const auto& t : tags) {
      output += "</";
      output += t;
      output += ">";
    }
    tags.clear();
  };

  auto append_trunc = [&](std::string piece) {
    // add_truncation_text(piece, suffix) with plain suffix (no %(truncated_text)s)
    if (!(piece.size() >= suffix.size() && !suffix.empty() &&
          piece.compare(piece.size() - suffix.size(), suffix.size(), suffix) == 0)) {
      piece += suffix;
    } else if (suffix.empty()) {
      // already ends with empty
    }
    // empty suffix: just use piece
    if (suffix.empty()) {
      // piece already correct without append when empty
    }
    // Fix: if suffix empty, don't check endswith
    output += piece;
  };

  auto handle_text_chars = [&](std::string_view raw_data) -> bool {
    std::string data = decode_charrefs(raw_data);
    processed_chars += utf8_len(data);
    // Exact-fit edge case (TruncateCharsHTMLParser.process): when total text
    // chars equal the user-facing length and this chunk finishes the input,
    // append unescaped data and skip the truncation suffix.
    if (processed_chars == original_length &&
        static_cast<int>(output.size() + data.size()) == static_cast<int>(html.size())) {
      output += data;
      return true;  // TruncationCompleted without suffix
    }
    const int data_len = utf8_len(data);
    std::string piece = html_escape(utf8_prefix(data, remaining));
    if (remaining < data_len) {
      // add_truncation_text
      if (suffix.empty() ||
          !(piece.size() >= suffix.size() &&
            piece.compare(piece.size() - suffix.size(), suffix.size(), suffix) == 0)) {
        piece += std::string(suffix);
      }
      output += piece;
      remaining = 0;
      return true;
    }
    remaining -= data_len;
    output += piece;
    return false;
  };

  auto handle_text_words = [&](std::string_view raw_data) -> bool {
    std::string data = decode_charrefs(raw_data);
    // re.split(r"(?<=\S)\s+(?=\S)", data) — split only on WS between non-WS
    std::vector<std::string> parts;
    if (!data.empty()) {
      std::size_t start = 0;
      for (std::size_t i = 0; i < data.size();) {
        // find whitespace run with non-ws before and after
        if ((data[i] == ' ' || data[i] == '\t' || data[i] == '\n' || data[i] == '\r' ||
             data[i] == '\f' || data[i] == '\v')) {
          // check lookbehind: previous is non-ws
          if (i > 0 &&
              !(data[i - 1] == ' ' || data[i - 1] == '\t' || data[i - 1] == '\n' ||
                data[i - 1] == '\r' || data[i - 1] == '\f' || data[i - 1] == '\v')) {
            std::size_t ws = i;
            while (ws < data.size() &&
                   (data[ws] == ' ' || data[ws] == '\t' || data[ws] == '\n' ||
                    data[ws] == '\r' || data[ws] == '\f' || data[ws] == '\v')) {
              ++ws;
            }
            if (ws < data.size() &&
                !(data[ws] == ' ' || data[ws] == '\t' || data[ws] == '\n' ||
                  data[ws] == '\r' || data[ws] == '\f' || data[ws] == '\v')) {
              // split here
              parts.emplace_back(data.substr(start, i - start));
              start = ws;
              i = ws;
              continue;
            }
          }
        }
        ++i;
      }
      parts.emplace_back(data.substr(start));
    }
    const int data_len = static_cast<int>(parts.size());
    std::string joined;
    const int take = std::min(remaining, data_len);
    for (int w = 0; w < take; ++w) {
      if (w) {
        joined += ' ';
      }
      joined += parts[static_cast<std::size_t>(w)];
    }
    std::string piece = html_escape(joined);
    if (remaining < data_len) {
      if (suffix.empty() ||
          !(piece.size() >= suffix.size() &&
            piece.compare(piece.size() - suffix.size(), suffix.size(), suffix) == 0)) {
        piece += std::string(suffix);
      }
      output += piece;
      remaining = 0;
      return true;
    }
    remaining -= data_len;
    output += piece;
    return false;
  };

  const std::size_t n = html.size();
  std::size_t i = 0;
  auto is_markup_start = [](char c) {
    // HTMLParser only enters markup for letter, '/', '!', '?'
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '/' || c == '!' ||
           c == '?';
  };
  while (i < n) {
    if (html[i] == '<') {
      if (i + 1 >= n || !is_markup_start(html[i + 1])) {
        // Lone '<' (HTMLParser handle_data("<"))
        bool done = by_words ? handle_text_words(html.substr(i, 1))
                             : handle_text_chars(html.substr(i, 1));
        ++i;
        if (done) {
          close_open();
          return output;
        }
        continue;
      }
      // Potential tag: scan to '>' respecting quotes
      std::size_t j = i + 1;
      char quote = 0;
      bool found_gt = false;
      while (j < n) {
        char c = html[j];
        if (quote) {
          if (c == quote) {
            quote = 0;
          }
        } else if (c == '"' || c == '\'') {
          quote = c;
        } else if (c == '>') {
          ++j;
          found_gt = true;
          break;
        }
        ++j;
      }
      if (!found_gt) {
        // Incomplete tag at EOF — HTMLParser discards (no handle_data).
        break;
      }
      std::string_view tag_text = html.substr(i, j - i);
      const std::string tag_name = extract_tag_name(tag_text);
      const bool end = is_end_tag(tag_text);
      const bool voidish =
          (!tag_name.empty() && is_void_element(tag_name)) || is_self_closing(tag_text);
      if (end) {
        if (!tag_name.empty() && !is_void_element(tag_name)) {
          output += "</";
          output += tag_name;
          output += ">";
          if (!tags.empty() && tags.front() == tag_name) {
            tags.pop_front();
          }
        }
        // void end tags: not emitted
      } else {
        output.append(tag_text);
        if (!tag_name.empty() && !voidish) {
          tags.push_front(tag_name);
        }
      }
      i = j;
      continue;
    }
    // Text until next '<'
    std::size_t j = i;
    while (j < n && html[j] != '<') {
      ++j;
    }
    std::string_view data = html.substr(i, j - i);
    if (!data.empty()) {
      bool done = by_words ? handle_text_words(data) : handle_text_chars(data);
      if (done) {
        close_open();
        return output;
      }
    }
    i = j;
  }
  return output;
}

// Expand IPv6 to 8 hextets; return false if invalid.
bool expand_ipv6(std::string_view ip, std::array<int, 8>& hextets) {
  if (ip.empty() || ip.size() > 45) {
    return false;
  }
  std::string s(ip);
  for (char& c : s) {
    if (c >= 'A' && c <= 'F') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  // Convert trailing IPv4 dotted-quad to two hextets
  if (s.find('.') != std::string::npos) {
    auto colon = s.rfind(':');
    if (colon == std::string::npos) {
      return false;
    }
    std::string v4 = s.substr(colon + 1);
    int o[4] = {0, 0, 0, 0};
    std::size_t st = 0;
    for (int k = 0; k < 4; ++k) {
      auto dot = v4.find('.', st);
      std::string part =
          dot == std::string::npos ? v4.substr(st) : v4.substr(st, dot - st);
      if (part.empty() || part.size() > 3) {
        return false;
      }
      int v = 0;
      for (char ch : part) {
        if (ch < '0' || ch > '9') {
          return false;
        }
        v = v * 10 + (ch - '0');
      }
      if (v > 255) {
        return false;
      }
      o[k] = v;
      st = dot == std::string::npos ? v4.size() : dot + 1;
      if (dot == std::string::npos && k < 3) {
        return false;
      }
    }
    if (st != v4.size()) {
      return false;
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%x:%x", (o[0] << 8) | o[1], (o[2] << 8) | o[3]);
    s = s.substr(0, colon + 1) + buf;
  }

  auto ddc = s.find("::");
  auto split_hextets = [](std::string_view part, std::vector<std::string>& out) -> bool {
    if (part.empty()) {
      return true;
    }
    // Leading/trailing empty from split means empty hextet — invalid unless from ::
    std::size_t st = 0;
    while (true) {
      auto col = part.find(':', st);
      std::string_view h =
          col == std::string_view::npos ? part.substr(st) : part.substr(st, col - st);
      if (h.empty() || h.size() > 4) {
        return false;
      }
      for (char ch : h) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
          return false;
        }
      }
      out.emplace_back(h);
      if (col == std::string_view::npos) {
        break;
      }
      st = col + 1;
      if (st > part.size()) {
        return false;
      }
    }
    return true;
  };

  std::vector<std::string> full;
  if (ddc != std::string::npos) {
    if (s.find("::", ddc + 1) != std::string::npos) {
      return false;  // multiple ::
    }
    // ":::x" invalid
    if (ddc + 2 < s.size() && s[ddc + 2] == ':') {
      return false;
    }
    std::vector<std::string> left, right;
    if (!split_hextets(std::string_view(s).substr(0, ddc), left)) {
      return false;
    }
    if (!split_hextets(std::string_view(s).substr(ddc + 2), right)) {
      return false;
    }
    const int n_h = static_cast<int>(left.size() + right.size());
    if (n_h > 7) {  // need at least one zero from ::
      // Actually :: can expand to fill to 8; n_h must be < 8
      if (n_h >= 8) {
        return false;
      }
    }
    if (n_h >= 8) {
      return false;
    }
    int fill = 8 - n_h;
    full = left;
    for (int i = 0; i < fill; ++i) {
      full.emplace_back("0");
    }
    full.insert(full.end(), right.begin(), right.end());
  } else {
    if (!split_hextets(s, full) || full.size() != 8) {
      return false;
    }
  }
  if (full.size() != 8) {
    return false;
  }
  for (int i = 0; i < 8; ++i) {
    hextets[static_cast<std::size_t>(i)] =
        std::stoi(full[static_cast<std::size_t>(i)], nullptr, 16);
  }
  return true;
}

std::string compress_ipv6(const std::array<int, 8>& hextets) {
  // Longest zero run (len >= 2); leftmost wins on ties (ipaddress).
  int best_start = -1, best_len = 0;
  int run_start = -1, run_len = 0;
  for (int i = 0; i < 8; ++i) {
    if (hextets[static_cast<std::size_t>(i)] == 0) {
      if (run_start < 0) {
        run_start = i;
        run_len = 1;
      } else {
        ++run_len;
      }
    } else {
      if (run_len > best_len) {
        best_len = run_len;
        best_start = run_start;
      }
      run_start = -1;
      run_len = 0;
    }
  }
  if (run_len > best_len) {
    best_len = run_len;
    best_start = run_start;
  }
  if (best_len < 2) {
    best_start = -1;
    best_len = 0;
  }

  std::string out;
  for (int i = 0; i < 8;) {
    if (best_start >= 0 && i == best_start) {
      out += "::";
      i += best_len;
      continue;
    }
    if (!out.empty() && out.back() != ':') {
      out += ':';
    }
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%x", hextets[static_cast<std::size_t>(i)]);
    out += buf;
    ++i;
  }
  if (out.empty()) {
    return "::";
  }
  return out;
}

}  // namespace

std::string truncate_chars_html(std::string_view html, int length,
                                std::string_view truncate_suffix) {
  return truncate_html_impl(html, length, truncate_suffix, false);
}

std::string truncate_words_html(std::string_view html, int length,
                                std::string_view truncate_suffix) {
  return truncate_html_impl(html, length, truncate_suffix, true);
}

std::optional<std::string> clean_ipv6_address(std::string_view ip, bool unpack_ipv4,
                                              int max_length) {
  if (static_cast<int>(ip.size()) > max_length) {
    return std::nullopt;
  }
  // Scope/zone id not accepted by Django's max-length path as pure IPv6 clean
  if (ip.find('%') != std::string_view::npos) {
    return std::nullopt;
  }
  std::array<int, 8> hextets{};
  if (!expand_ipv6(ip, hextets)) {
    return std::nullopt;
  }
  // IPv4-mapped: ::ffff:x:x
  if (hextets[0] == 0 && hextets[1] == 0 && hextets[2] == 0 && hextets[3] == 0 &&
      hextets[4] == 0 && hextets[5] == 0xffff) {
    int a = (hextets[6] >> 8) & 0xff;
    int b = hextets[6] & 0xff;
    int c = (hextets[7] >> 8) & 0xff;
    int d = hextets[7] & 0xff;
    char buf[48];
    if (unpack_ipv4) {
      std::snprintf(buf, sizeof(buf), "%d.%d.%d.%d", a, b, c, d);
    } else {
      std::snprintf(buf, sizeof(buf), "::ffff:%d.%d.%d.%d", a, b, c, d);
    }
    return std::string(buf);
  }
  return compress_ipv6(hextets);
}

std::string render_fortune_page(
    const std::vector<std::pair<std::int64_t, std::string>>& rows) {
  // Match TechEmpower Django fortunes.html + base.html structure closely.
  static constexpr std::string_view kPrefix =
      "<!DOCTYPE html>\n"
      "<html>\n"
      "<head>\n"
      "<title>Fortunes</title>\n"
      "</head>\n"
      "<body>\n"
      "  \n"
      "<table>\n"
      "<tr>\n"
      "<th>id</th>\n"
      "<th>message</th>\n"
      "</tr>\n";
  static constexpr std::string_view kSuffix =
      "</table>\n"
      "\n"
      "</body>\n"
      "</html>";

  std::size_t reserve = kPrefix.size() + kSuffix.size();
  for (const auto& [id, msg] : rows) {
    // <tr>\n<td>…</td>\n<td>…</td>\n</tr>\n + worst-case escape overhead
    reserve += 32 + 20 + msg.size() * 2;
    (void)id;
  }
  std::string out;
  out.reserve(reserve);
  out.append(kPrefix);
  for (const auto& [id, msg] : rows) {
    out.append("<tr>\n<td>");
    out.append(std::to_string(id));
    out.append("</td>\n<td>");
    // Inline escape into out for fewer allocations.
    for (char c : msg) {
      switch (c) {
        case '&':
          out += "&amp;";
          break;
        case '<':
          out += "&lt;";
          break;
        case '>':
          out += "&gt;";
          break;
        case '"':
          out += "&quot;";
          break;
        case '\'':
          out += "&#x27;";
          break;
        default:
          out += c;
          break;
      }
    }
    out.append("</td>\n</tr>\n");
  }
  out.append(kSuffix);
  return out;
}

}  // namespace django::native
