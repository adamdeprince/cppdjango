#include "urlize.hpp"

#include "text.hpp"        // truncate_chars/words, querydict_urlencode
#include "validators.hpp"  // is_valid_email

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace django::native {

std::string trim_url(std::string_view url, int limit) {
  if (limit < 0) {
    return std::string(url);
  }
  // limit is None handled in Python; here limit is always set.
  if (static_cast<int>(url.size()) <= limit) {
    // Note: len() is code units in Python str; for UTF-8 we use byte length
    // only when input is ASCII. Caller passes NFC text; for non-ASCII use
    // codepoint count.
    int cps = 0;
    for (std::size_t i = 0; i < url.size();) {
      auto lead = static_cast<unsigned char>(url[i]);
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
    if (cps <= limit) {
      return std::string(url);
    }
  }
  // x[:max(0, limit-1)] + "…"
  const int keep = limit > 0 ? limit - 1 : 0;
  if (keep <= 0) {
    return "…";
  }
  // Take first `keep` codepoints
  std::size_t i = 0;
  int cps = 0;
  while (i < url.size() && cps < keep) {
    auto lead = static_cast<unsigned char>(url[i]);
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
  return std::string(url.substr(0, i)) + "…";
}

std::vector<std::string> urlize_word_split(std::string_view text) {
  // ([\s<>"']+)
  std::vector<std::string> out;
  auto is_sep = [](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v' ||
           c == '<' || c == '>' || c == '"' || c == '\'';
  };
  std::size_t i = 0;
  while (i < text.size()) {
    if (is_sep(text[i])) {
      std::size_t j = i;
      while (j < text.size() && is_sep(text[j])) {
        ++j;
      }
      out.emplace_back(text.substr(i, j - i));
      i = j;
    } else {
      std::size_t j = i;
      while (j < text.size() && !is_sep(text[j])) {
        ++j;
      }
      out.emplace_back(text.substr(i, j - i));
      i = j;
    }
  }
  return out;
}

TrimPunct trim_urlize_punctuation(std::string_view word) {
  TrimPunct r;
  std::string middle(word);
  // Strip opening ( [
  while (!middle.empty() && (middle.front() == '(' || middle.front() == '[')) {
    r.lead += middle.front();
    middle.erase(0, 1);
  }
  // Strip trailing .,;:! and unmatched ) ]
  auto count = [](const std::string& s, char c) {
    return static_cast<int>(std::count(s.begin(), s.end(), c));
  };
  bool changed = true;
  while (changed && !middle.empty()) {
    changed = false;
    if (count(middle, '(') < count(middle, ')')) {
      // rstrip one )
      if (middle.back() == ')') {
        r.trail = ")" + r.trail;
        middle.pop_back();
        changed = true;
        continue;
      }
    }
    if (count(middle, '[') < count(middle, ']')) {
      if (middle.back() == ']') {
        r.trail = "]" + r.trail;
        middle.pop_back();
        changed = true;
        continue;
      }
    }
    // trailing punctuation .,;:!
    const char back = middle.back();
    if (back == '.' || back == ',' || back == ':' || back == '!' || back == ';') {
      // Don't strip ; if part of entity — simplified: always strip for native
      r.trail = std::string(1, back) + r.trail;
      middle.pop_back();
      changed = true;
    }
  }
  r.middle = std::move(middle);
  return r;
}

bool urlize_is_email_simple(std::string_view value) {
  return is_valid_email(value, {});  // empty allowlist
}

namespace {

[[nodiscard]] bool is_ascii_alnum(unsigned char c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

[[nodiscard]] bool is_word_char(unsigned char c) noexcept {
  // Python \w with re.ASCII not set — Unicode word. For URL start we only need
  // first char after optional '[': simple_url_re uses \w which includes digits/letters.
  return is_ascii_alnum(c) || c == '_' || c >= 0x80;
}

// Label chars for hostname (ASCII path).
[[nodiscard]] bool is_host_label_body(unsigned char c) noexcept {
  return is_ascii_alnum(c) || c == '-' || c >= 0x80;
}

// Match DomainNameValidator.hostname_re roughly for ASCII+IDN labels.
// [a-z0-9\u00a1-\uffff](?:[a-z0-9-\u00a1-\uffff]{0,61}[a-z0-9\u00a1-\uffff])?
bool match_hostname_label(std::string_view s, std::size_t& i) {
  if (i >= s.size()) {
    return false;
  }
  auto lead = static_cast<unsigned char>(s[i]);
  // Must start with alnum or non-ASCII (not hyphen)
  if (!((lead >= '0' && lead <= '9') || (lead >= 'A' && lead <= 'Z') ||
        (lead >= 'a' && lead <= 'z') || lead >= 0x80)) {
    return false;
  }
  std::size_t start = i;
  ++i;
  while (i < s.size() && is_host_label_body(static_cast<unsigned char>(s[i]))) {
    ++i;
  }
  // Max label length 63 octets (approx code units for ASCII; loose for UTF-8)
  if (i - start > 63) {
    return false;
  }
  // Can't end with hyphen (if last char is ASCII '-')
  if (i > start && s[i - 1] == '-') {
    return false;
  }
  return true;
}

bool is_classic_gtld(std::string_view tld) {
  static constexpr const char* kG[] = {"com", "edu", "gov", "int", "mil", "net", "org"};
  std::string lower;
  lower.reserve(tld.size());
  for (char c : tld) {
    if (c >= 'A' && c <= 'Z') {
      lower += static_cast<char>(c - 'A' + 'a');
    } else {
      lower += c;
    }
  }
  for (const char* g : kG) {
    if (lower == g) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool urlize_simple_url_match(std::string_view middle) noexcept {
  // ^https?://\[?\w
  if (middle.size() < 8) {
    return false;
  }
  auto starts = [](std::string_view s, const char* pfx) {
    std::size_t n = 0;
    while (pfx[n]) {
      ++n;
    }
    if (s.size() < n) {
      return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
      char a = s[i];
      char b = pfx[i];
      if (a >= 'A' && a <= 'Z') {
        a = static_cast<char>(a - 'A' + 'a');
      }
      if (a != b) {
        return false;
      }
    }
    return true;
  };
  std::size_t off = 0;
  if (starts(middle, "https://")) {
    off = 8;
  } else if (starts(middle, "http://")) {
    off = 7;
  } else {
    return false;
  }
  if (off < middle.size() && middle[off] == '[') {
    ++off;
  }
  if (off >= middle.size()) {
    return false;
  }
  return is_word_char(static_cast<unsigned char>(middle[off]));
}

bool urlize_simple_url_2_match(std::string_view middle) noexcept {
  // Python: ^www\. | ^(?!http) hostname domain_re \.(com|edu|...)($|/.*)$
  // First alternative matches any string starting with "www." (re.match prefix).
  if (middle.size() >= 4) {
    char c0 = middle[0], c1 = middle[1], c2 = middle[2], c3 = middle[3];
    if (c0 >= 'A' && c0 <= 'Z') c0 = static_cast<char>(c0 - 'A' + 'a');
    if (c1 >= 'A' && c1 <= 'Z') c1 = static_cast<char>(c1 - 'A' + 'a');
    if (c2 >= 'A' && c2 <= 'Z') c2 = static_cast<char>(c2 - 'A' + 'a');
    if (c0 == 'w' && c1 == 'w' && c2 == 'w' && c3 == '.') {
      return true;
    }
  }
  if (middle.empty()) {
    return false;
  }
  // (?!http) — reject if starts with "http" (case-insensitive)
  if (middle.size() >= 4) {
    char a = middle[0], b = middle[1], c = middle[2], d = middle[3];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (d >= 'A' && d <= 'Z') d = static_cast<char>(d - 'A' + 'a');
    if (a == 'h' && b == 't' && c == 't' && d == 'p') {
      return false;
    }
  }

  const std::size_t path = middle.find('/');
  std::string_view hostpart =
      path == std::string_view::npos ? middle : middle.substr(0, path);
  auto last_dot = hostpart.rfind('.');
  if (last_dot == std::string_view::npos || last_dot == 0) {
    return false;
  }
  if (!is_classic_gtld(hostpart.substr(last_dot + 1))) {
    return false;
  }
  std::size_t p = 0;
  bool any = false;
  while (p < last_dot) {
    if (any) {
      if (hostpart[p] != '.') {
        return false;
      }
      ++p;
    }
    if (!match_hostname_label(hostpart, p)) {
      return false;
    }
    any = true;
    if (p == last_dot) {
      break;
    }
    if (p > last_dot) {
      return false;
    }
  }
  return any;
}

DecimalDigitCounts decimal_digit_counts(std::string_view digits, int exponent) {
  DecimalDigitCounts c;
  // exponent special values not represented as int here — Python checks first.
  if (exponent >= 0) {
    c.digits = static_cast<int>(digits.size());
    if (!(digits.size() == 1 && digits[0] == '0')) {
      c.digits += exponent;
    }
    c.decimals = 0;
  } else {
    const int abs_exp = -exponent;
    if (abs_exp > static_cast<int>(digits.size())) {
      c.digits = c.decimals = abs_exp;
    } else {
      c.digits = static_cast<int>(digits.size());
      c.decimals = abs_exp;
    }
  }
  c.whole_digits = c.digits - c.decimals;
  return c;
}

std::string sanitize_separators_ascii(std::string_view value,
                                      std::string_view decimal_sep,
                                      std::string_view thousand_sep,
                                      bool use_thousand) {
  std::string v(value);
  std::string decimals;
  if (!decimal_sep.empty()) {
    auto pos = v.find(decimal_sep);
    if (pos != std::string::npos) {
      decimals = v.substr(pos + decimal_sep.size());
      v = v.substr(0, pos);
    }
  }
  if (use_thousand && !thousand_sep.empty()) {
    // Special case: single '.' thousand that might be decimal
    if (thousand_sep == "." && v.find('.') != std::string::npos) {
      auto parts = v.find('.');
      auto last = v.rfind('.');
      if (parts == last && v.size() - last - 1 != 3) {
        // leave dots
      } else {
        std::string nv;
        for (std::size_t i = 0; i < v.size();) {
          if (v.compare(i, thousand_sep.size(), thousand_sep) == 0) {
            i += thousand_sep.size();
          } else {
            nv += v[i++];
          }
        }
        v = std::move(nv);
      }
    } else {
      std::string nv;
      for (std::size_t i = 0; i < v.size();) {
        if (v.compare(i, thousand_sep.size(), thousand_sep) == 0) {
          i += thousand_sep.size();
        } else {
          nv += v[i++];
        }
      }
      v = std::move(nv);
    }
  }
  if (!decimals.empty()) {
    return v + "." + decimals;
  }
  return v;
}

std::string querydict_urlencode_bytes(
    const std::vector<std::pair<std::string, std::string>>& pairs,
    std::string_view safe) {
  // Quote each raw byte (latin-1 view of charset-encoded data). Matches
  // urllib.parse.quote on bytes for non-UTF-8 QueryDict encodings.
  auto quote_bytes = [&](std::string_view raw) {
    std::string out;
    out.reserve(raw.size() * 3);
    auto hex = [](unsigned v) {
      return static_cast<char>(v < 10 ? '0' + v : 'A' + (v - 10));
    };
    for (unsigned char c : raw) {
      const bool unreserved =
          (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '-' || c == '.' || c == '_' || c == '~';
      if (unreserved || safe.find(static_cast<char>(c)) != std::string_view::npos) {
        out += static_cast<char>(c);
      } else {
        out += '%';
        out += hex(c >> 4);
        out += hex(c & 0xF);
      }
    }
    return out;
  };
  std::string out;
  for (std::size_t i = 0; i < pairs.size(); ++i) {
    if (i) {
      out += '&';
    }
    out += quote_bytes(pairs[i].first);
    out += '=';
    out += quote_bytes(pairs[i].second);
  }
  return out;
}

}  // namespace django::native
