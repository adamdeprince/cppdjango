#include "validators.hpp"

#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace django::native {
namespace {

[[nodiscard]] bool is_digit(char c) noexcept {
  return c >= '0' && c <= '9';
}

[[nodiscard]] bool is_alpha(char c) noexcept {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

[[nodiscard]] bool is_slug_char(char c) noexcept {
  return is_digit(c) || is_alpha(c) || c == '-' || c == '_';
}

// Email user atom chars: [-!#$%&'*+/=?^_`{}|~0-9A-Za-z]
[[nodiscard]] bool is_email_atom_char(unsigned char c) noexcept {
  if (is_digit(static_cast<char>(c)) || is_alpha(static_cast<char>(c))) {
    return true;
  }
  switch (c) {
    case '-':
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '/':
    case '=':
    case '?':
    case '^':
    case '_':
    case '`':
    case '{':
    case '}':
    case '|':
    case '~':
      return true;
    default:
      return false;
  }
}

// Domain label: [a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])? case-insensitive ASCII
// path; for non-ASCII we accept letter-ish UTF-8 bytes loosely for IDN labels.
[[nodiscard]] bool is_domain_label_char(unsigned char c, bool& ascii_only) noexcept {
  if (c >= 0x80) {
    ascii_only = false;
    return true;  // multi-byte UTF-8 continuation handled by caller as opaque
  }
  char ch = static_cast<char>(c);
  if (is_digit(ch) || is_alpha(ch) || ch == '-') {
    return true;
  }
  return false;
}

[[nodiscard]] bool match_email_user(std::string_view user) noexcept {
  if (user.empty()) {
    return false;
  }
  // Quoted-string form: "..."
  // Django: "([\001-\010\013\014\016-\037!#-\[\]-\177]|\\[\001-\011\013\014\016-\177])*"
  if (user.front() == '"' && user.back() == '"' && user.size() >= 2) {
    auto allowed_plain = [](unsigned char c) {
      // \001-\010, \013, \014, \016-\037, !#-\[\]-\177 (no DEL, no " unescaped)
      if (c >= 1 && c <= 8) {
        return true;
      }
      if (c == 0x0B || c == 0x0C) {
        return true;
      }
      if (c >= 0x0E && c <= 0x1F) {
        return true;
      }
      if (c >= 0x21 && c <= 0x7E && c != '"') {  // ! through ~ except "
        // also exclude [ might be in range - actually !#-\[\] means
        // ! # - [ ] through DEL-1... simplified: printable except " and \
        return c != '\\';
      }
      return false;
    };
    auto allowed_escaped = [](unsigned char c) {
      // \\[\001-\011\013\014\016-\177]
      if (c >= 1 && c <= 9) {
        return true;
      }
      if (c == 0x0B || c == 0x0C) {
        return true;
      }
      if (c >= 0x0E && c <= 0x7F) {
        return true;
      }
      return false;
    };
    for (std::size_t i = 1; i + 1 < user.size(); ++i) {
      unsigned char c = static_cast<unsigned char>(user[i]);
      if (c == '\\') {
        if (i + 1 >= user.size() - 1) {
          return false;
        }
        unsigned char n = static_cast<unsigned char>(user[i + 1]);
        if (!allowed_escaped(n)) {
          return false;
        }
        ++i;
        continue;
      }
      if (!allowed_plain(c)) {
        return false;
      }
    }
    return true;
  }
  // dot-atom: atom(\.atom)*
  std::size_t i = 0;
  auto atom = [&]() -> bool {
    if (i >= user.size() || !is_email_atom_char(static_cast<unsigned char>(user[i]))) {
      return false;
    }
    while (i < user.size() &&
           is_email_atom_char(static_cast<unsigned char>(user[i]))) {
      ++i;
    }
    return true;
  };
  if (!atom()) {
    return false;
  }
  while (i < user.size()) {
    if (user[i] != '.') {
      return false;
    }
    ++i;
    if (!atom()) {
      return false;
    }
  }
  return true;
}

// Domain matching EmailValidator.domain_regex (hostname + domain* + tld).
// TLD: [a-z-]{2,63} or xn--[a-z0-9]{1,59} (no digits in normal TLD).
[[nodiscard]] bool match_domain_ascii(std::string_view domain) noexcept {
  if (domain.empty() || domain.size() > 253) {
    return false;
  }
  std::vector<std::string_view> labels;
  std::size_t i = 0;
  while (i < domain.size()) {
    if (domain[i] == '.' || domain[i] == '-') {
      return false;
    }
    std::size_t start = i;
    while (i < domain.size() && domain[i] != '.') {
      unsigned char c = static_cast<unsigned char>(domain[i]);
      char ch = domain[i];
      if (!(is_digit(ch) || is_alpha(ch) || ch == '-' || c >= 0x80)) {
        return false;
      }
      ++i;
    }
    std::size_t len = i - start;
    if (len == 0 || len > 63) {
      return false;
    }
    if (domain[start + len - 1] == '-') {
      return false;
    }
    labels.push_back(domain.substr(start, len));
    if (i < domain.size() && domain[i] == '.') {
      ++i;
      if (i == domain.size()) {
        return false;  // trailing dot
      }
    }
  }
  if (labels.size() < 2) {
    return false;
  }
  std::string_view tld = labels.back();
  // punycode TLD
  if (tld.size() >= 4 && (tld[0] == 'x' || tld[0] == 'X') &&
      (tld[1] == 'n' || tld[1] == 'N') && tld[2] == '-' && tld[3] == '-') {
    for (std::size_t k = 4; k < tld.size(); ++k) {
      char ch = tld[k];
      if (!(is_digit(ch) || is_alpha(ch))) {
        return false;
      }
    }
    return tld.size() <= 63;  // xn-- + up to 59
  }
  // Normal TLD: letters and hyphens only, length 2-63 (no digits).
  if (tld.size() < 2 || tld.size() > 63) {
    return false;
  }
  for (char ch : tld) {
    unsigned char c = static_cast<unsigned char>(ch);
    if (c >= 0x80) {
      continue;  // IDN
    }
    if (!(is_alpha(ch) || ch == '-')) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool parse_ipv4_octet(std::string_view s, int& out) noexcept {
  if (s.empty() || s.size() > 3) {
    return false;
  }
  // Disallow leading zeros like 00, 01 except single 0 — ipaddress accepts
  // "0" and rejects ambiguous forms; Python ipaddress allows "0" only for zero.
  // Actually IPv4Address('01') fails in Python 3. Actually:
  // IPv4Address('127.0.0.01') works in some versions... Django uses ipaddress.
  // Strict: no empty, digits only, 0-255. Leading zeros: Python 3.14
  // ipaddress.IPv4Address('01') raises. So reject leading zero if len>1.
  if (s.size() > 1 && s[0] == '0') {
    return false;
  }
  int v = 0;
  for (char c : s) {
    if (!is_digit(c)) {
      return false;
    }
    v = v * 10 + (c - '0');
  }
  if (v > 255) {
    return false;
  }
  out = v;
  return true;
}

[[nodiscard]] int hex_val(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

}  // namespace

bool is_valid_slug(std::string_view value) noexcept {
  if (value.empty()) {
    return false;
  }
  for (char c : value) {
    if (!is_slug_char(c)) {
      return false;
    }
  }
  return true;
}

bool is_valid_integer_string(std::string_view value) noexcept {
  if (value.empty()) {
    return false;
  }
  std::size_t i = 0;
  if (value[0] == '-') {
    if (value.size() == 1) {
      return false;
    }
    i = 1;
  }
  for (; i < value.size(); ++i) {
    if (!is_digit(value[i])) {
      return false;
    }
  }
  return true;
}

std::optional<std::int64_t> form_integer_to_python(std::string_view value) {
  // Match IntegerField: re.sub(r"\.0*\s*$", "", str(value)) then int().
  std::string s(value);
  // Apply \.0*\s*$ removal from the end.
  std::size_t end = s.size();
  while (end > 0 &&
         (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\n' ||
          s[end - 1] == '\r')) {
    --end;
  }
  std::size_t z = end;
  while (z > 0 && s[z - 1] == '0') {
    --z;
  }
  if (z > 0 && s[z - 1] == '.') {
    end = z - 1;
  }
  s.resize(end);

  const char* b = s.data();
  const char* e = s.data() + s.size();
  while (b < e && (*b == ' ' || *b == '\t')) {
    ++b;
  }
  if (b < e && *b == '+') {
    ++b;
  }
  if (b >= e) {
    return std::nullopt;
  }
  long long result = 0;
  auto [ptr, ec] = std::from_chars(b, e, result);
  if (ec != std::errc{}) {
    return std::nullopt;
  }
  while (ptr < e && (*ptr == ' ' || *ptr == '\t')) {
    ++ptr;
  }
  if (ptr != e) {
    return std::nullopt;
  }
  return result;
}

bool is_valid_ipv4(std::string_view value) noexcept {
  int parts[4];
  std::size_t start = 0;
  for (int p = 0; p < 4; ++p) {
    if (start >= value.size()) {
      return false;
    }
    std::size_t end = value.find('.', start);
    if (p < 3) {
      if (end == std::string_view::npos) {
        return false;
      }
      if (!parse_ipv4_octet(value.substr(start, end - start), parts[p])) {
        return false;
      }
      start = end + 1;
    } else {
      if (end != std::string_view::npos) {
        return false;
      }
      if (!parse_ipv4_octet(value.substr(start), parts[p])) {
        return false;
      }
    }
  }
  return true;
}

bool is_valid_ipv6(std::string_view value) noexcept {
  // Accept forms that ipaddress.IPv6Address typically accepts.
  if (value.empty() || value.size() > 45) {
    return false;
  }
  // IPv4-mapped: ...:dotted
  std::string_view v = value;
  std::string storage;
  auto dot = v.rfind('.');
  if (dot != std::string_view::npos) {
    // last hextet may be IPv4
    auto colon = v.rfind(':');
    if (colon == std::string_view::npos) {
      return false;
    }
    std::string_view ipv4 = v.substr(colon + 1);
    if (!is_valid_ipv4(ipv4)) {
      return false;
    }
    // convert ipv4 to two hextets for counting
    int o[4];
    std::size_t st = 0;
    for (int i = 0; i < 4; ++i) {
      auto e = ipv4.find('.', st);
      std::string_view part =
          e == std::string_view::npos ? ipv4.substr(st) : ipv4.substr(st, e - st);
      parse_ipv4_octet(part, o[i]);
      st = e == std::string_view::npos ? ipv4.size() : e + 1;
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%x:%x", (o[0] << 8) | o[1], (o[2] << 8) | o[3]);
    storage.assign(v.substr(0, colon + 1));
    storage += buf;
    v = storage;
  }

  // Count '::' — at most one
  int empty_runs = 0;
  std::size_t i = 0;
  int hextets = 0;
  if (!v.empty() && v[0] == ':') {
    if (v.size() < 2 || v[1] != ':') {
      return false;
    }
  }
  while (i < v.size()) {
    if (v[i] == ':') {
      if (i + 1 < v.size() && v[i + 1] == ':') {
        ++empty_runs;
        if (empty_runs > 1) {
          return false;
        }
        i += 2;
        if (i >= v.size()) {
          break;  // trailing ::
        }
        continue;
      }
      if (i == 0) {
        return false;  // leading single :
      }
      ++i;
      continue;
    }
    // hextet 1-4 hex digits
    int digits = 0;
    while (i < v.size() && v[i] != ':') {
      if (hex_val(v[i]) < 0) {
        return false;
      }
      ++digits;
      ++i;
      if (digits > 4) {
        return false;
      }
    }
    if (digits == 0) {
      return false;
    }
    ++hextets;
  }
  if (empty_runs == 1) {
    return hextets < 8;
  }
  return hextets == 8;
}

bool is_valid_ipv46(std::string_view value) noexcept {
  return is_valid_ipv4(value) || is_valid_ipv6(value);
}

bool is_valid_email(std::string_view value,
                    const std::vector<std::string>& domain_allowlist) {
  if (value.empty() || value.size() > 320) {
    return false;
  }
  auto at = value.rfind('@');
  if (at == std::string_view::npos || at == 0 || at + 1 >= value.size()) {
    return false;
  }
  // Only one @ for unquoted local part — rsplit is correct for quoted too if
  // @ appears in quotes... Django uses rsplit("@", 1).
  std::string_view user = value.substr(0, at);
  std::string_view domain = value.substr(at + 1);
  if (!match_email_user(user)) {
    return false;
  }
  for (const auto& allowed : domain_allowlist) {
    if (domain == allowed) {
      return true;
    }
  }
  // Literal [IPv4/IPv6]
  if (domain.size() >= 2 && domain.front() == '[' && domain.back() == ']') {
    std::string_view inner = domain.substr(1, domain.size() - 2);
    return is_valid_ipv46(inner);
  }
  return match_domain_ascii(domain);
}

bool has_null_characters(std::string_view value) noexcept {
  return value.find('\0') != std::string_view::npos;
}

std::string char_field_strip(std::string_view value, bool strip) {
  std::string s(value);
  if (!strip) {
    return s;
  }
  std::size_t a = 0, b = s.size();
  while (a < b &&
         (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r' || s[a] == '\f' ||
          s[a] == '\v')) {
    ++a;
  }
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' ||
                   s[b - 1] == '\r' || s[b - 1] == '\f' || s[b - 1] == '\v')) {
    --b;
  }
  return s.substr(a, b - a);
}

namespace {

// Domain label: start/end alnum (or non-ASCII when idna), body alnum/hyphen.
// max 63 chars. ASCII-only when !accept_idna.
bool domain_label_ok(std::string_view lab, bool accept_idna) noexcept {
  if (lab.empty() || lab.size() > 63) {
    return false;
  }
  auto ok_start = [&](unsigned char c) {
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
      return true;
    }
    return accept_idna && c >= 0x80;
  };
  auto ok_body = [&](unsigned char c) {
    if (ok_start(c) || c == '-') {
      return true;
    }
    return accept_idna && c >= 0x80;
  };
  if (!ok_start(static_cast<unsigned char>(lab.front()))) {
    return false;
  }
  if (!ok_start(static_cast<unsigned char>(lab.back())) && lab.back() != '-') {
    // TLD rules differ slightly; hostname can't end with -
  }
  if (lab.front() == '-' || lab.back() == '-') {
    return false;
  }
  for (unsigned char c : lab) {
    if (!ok_body(c)) {
      return false;
    }
    if (!accept_idna && c >= 0x80) {
      return false;
    }
  }
  return true;
}

bool tld_label_ok(std::string_view lab, bool accept_idna) noexcept {
  // tld_no_fqdn_re: \.(?!-)(?:[a-z\u-]{2,63}|xn--[a-z0-9]{1,59})(?<!-)
  // ascii_only: [a-zA-Z0-9-]{2,63}
  if (lab.size() < 2 || lab.size() > 63) {
    return false;
  }
  if (lab.front() == '-' || lab.back() == '-') {
    return false;
  }
  // Prefer exact punycode alternative when it fully matches.
  if (accept_idna && lab.size() >= 5 && (lab[0] == 'x' || lab[0] == 'X') &&
      (lab[1] == 'n' || lab[1] == 'N') && lab[2] == '-' && lab[3] == '-') {
    bool pure_puny = true;
    for (std::size_t i = 4; i < lab.size(); ++i) {
      unsigned char c = static_cast<unsigned char>(lab[i]);
      if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
        pure_puny = false;
        break;
      }
    }
    // xn-- + 1..59 chars
    if (pure_puny && lab.size() >= 5 && lab.size() <= 63) {
      return true;
    }
    // e.g. xn---c falls through to general [a-z-]{2,63}
  }
  for (unsigned char c : lab) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-') {
      continue;
    }
    if (accept_idna && c >= 0x80) {
      continue;
    }
    // ascii_only_tld allows digits; IDNA non-punycode TLD does not.
    if (!accept_idna && c >= '0' && c <= '9') {
      continue;
    }
    return false;
  }
  return true;
}

}  // namespace

bool is_valid_domain_name(std::string_view value, bool accept_idna,
                          int max_length) noexcept {
  if (value.empty() || static_cast<int>(value.size()) > max_length) {
    return false;
  }
  if (!accept_idna) {
    for (unsigned char c : value) {
      if (c >= 0x80) {
        return false;
      }
    }
  }
  // Optional trailing dot
  std::string_view v = value;
  if (!v.empty() && v.back() == '.') {
    v.remove_suffix(1);
  }
  if (v.empty()) {
    return false;
  }
  // Split labels
  std::vector<std::string_view> labels;
  std::size_t st = 0;
  for (std::size_t i = 0; i <= v.size(); ++i) {
    if (i == v.size() || v[i] == '.') {
      if (i == st) {
        return false;  // empty label
      }
      labels.emplace_back(v.substr(st, i - st));
      st = i + 1;
    }
  }
  if (labels.size() < 2) {
    // DomainNameValidator requires hostname + tld (at least one dot)
    return false;
  }
  // All but last: hostname/domain labels
  for (std::size_t i = 0; i + 1 < labels.size(); ++i) {
    if (!domain_label_ok(labels[i], accept_idna)) {
      return false;
    }
  }
  // TLD
  if (!tld_label_ok(labels.back(), accept_idna)) {
    // ascii_only allows digits in TLD
    if (!accept_idna) {
      // re-check with digit-friendly
      const auto& lab = labels.back();
      if (lab.size() < 2 || lab.size() > 63 || lab.front() == '-' || lab.back() == '-') {
        return false;
      }
      for (unsigned char c : lab) {
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              c == '-')) {
          return false;
        }
      }
      return true;
    }
    return false;
  }
  return true;
}

bool url_structure_precheck(std::string_view value, int max_length,
                            std::string_view schemes_csv) noexcept {
  if (static_cast<int>(value.size()) > max_length || value.empty()) {
    return false;
  }
  for (char c : value) {
    if (c == '\t' || c == '\r' || c == '\n') {
      return false;
    }
  }
  auto sep = value.find("://");
  if (sep == std::string_view::npos || sep == 0) {
    return false;
  }
  std::string scheme;
  scheme.reserve(sep);
  for (std::size_t i = 0; i < sep; ++i) {
    char c = value[i];
    if (c >= 'A' && c <= 'Z') {
      scheme += static_cast<char>(c - 'A' + 'a');
    } else {
      scheme += c;
    }
  }
  // schemes_csv is comma-separated lowercase schemes
  std::size_t st = 0;
  bool found = false;
  while (st <= schemes_csv.size()) {
    auto comma = schemes_csv.find(',', st);
    std::string_view part = comma == std::string_view::npos
                                ? schemes_csv.substr(st)
                                : schemes_csv.substr(st, comma - st);
    // trim spaces
    while (!part.empty() && part.front() == ' ') {
      part.remove_prefix(1);
    }
    while (!part.empty() && part.back() == ' ') {
      part.remove_suffix(1);
    }
    if (part == scheme) {
      found = true;
      break;
    }
    if (comma == std::string_view::npos) {
      break;
    }
    st = comma + 1;
  }
  if (!found) {
    return false;
  }
  // Need some host-ish content after ://
  if (sep + 3 >= value.size()) {
    return false;
  }
  return true;
}

}  // namespace django::native
