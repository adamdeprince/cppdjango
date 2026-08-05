#include "request_utils.hpp"

#include "filters.hpp"  // url_quote

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace django::native {
namespace {

constexpr std::array<std::string_view, 12> kMonths = {
    "jan", "feb", "mar", "apr", "may", "jun",
    "jul", "aug", "sep", "oct", "nov", "dec",
};

constexpr std::array<std::string_view, 7> kWdays = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat",
};

constexpr std::array<std::string_view, 12> kMonNames = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};

[[nodiscard]] bool is_digit(char c) noexcept {
  return c >= '0' && c <= '9';
}

[[nodiscard]] bool is_alpha(char c) noexcept {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

[[nodiscard]] char to_lower(char c) noexcept {
  if (c >= 'A' && c <= 'Z') {
    return static_cast<char>(c - 'A' + 'a');
  }
  return c;
}

[[nodiscard]] std::string ascii_lower(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    out += to_lower(c);
  }
  return out;
}

[[nodiscard]] bool parse_n_digits(std::string_view s, std::size_t& i, int n,
                                  int& out) {
  if (i + static_cast<std::size_t>(n) > s.size()) {
    return false;
  }
  int v = 0;
  for (int k = 0; k < n; ++k) {
    if (!is_digit(s[i + static_cast<std::size_t>(k)])) {
      return false;
    }
    v = v * 10 + (s[i + static_cast<std::size_t>(k)] - '0');
  }
  i += static_cast<std::size_t>(n);
  out = v;
  return true;
}

// day can be " 6" or "06" for asctime
[[nodiscard]] bool parse_day_flex(std::string_view s, std::size_t& i, int& out) {
  if (i >= s.size()) {
    return false;
  }
  if (s[i] == ' ') {
    ++i;
    if (i >= s.size() || !is_digit(s[i])) {
      return false;
    }
    out = s[i] - '0';
    ++i;
    return true;
  }
  return parse_n_digits(s, i, 2, out);
}

[[nodiscard]] int month_index(std::string_view mon3) {
  if (mon3.size() != 3) {
    return -1;
  }
  char buf[3] = {to_lower(mon3[0]), to_lower(mon3[1]), to_lower(mon3[2])};
  std::string_view m(buf, 3);
  for (int i = 0; i < 12; ++i) {
    if (kMonths[static_cast<std::size_t>(i)] == m) {
      return i + 1;
    }
  }
  return -1;
}

[[nodiscard]] bool parse_time_hms(std::string_view s, std::size_t& i, int& hour,
                                  int& min, int& sec) {
  return parse_n_digits(s, i, 2, hour) && i < s.size() && s[i++] == ':' &&
         parse_n_digits(s, i, 2, min) && i < s.size() && s[i++] == ':' &&
         parse_n_digits(s, i, 2, sec);
}

// Convert calendar fields (UTC) to unix timestamp.
[[nodiscard]] std::optional<std::int64_t> to_unix(int year, int month, int day,
                                                  int hour, int min, int sec) {
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 ||
      min < 0 || min > 59 || sec < 0 || sec > 60) {
    return std::nullopt;
  }
  std::tm tm{};
  tm.tm_year = year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = min;
  tm.tm_sec = sec;
  tm.tm_isdst = 0;
#if defined(_WIN32)
  std::time_t t = _mkgmtime(&tm);
#else
  std::time_t t = timegm(&tm);
#endif
  if (t == static_cast<std::time_t>(-1)) {
    // Ambiguous; try checking fields
    return std::nullopt;
  }
  return static_cast<std::int64_t>(t);
}

// RFC 1123: Sun, 06 Nov 1994 08:49:37 GMT
// ^\w{3}, DD Mon YYYY HH:MM:SS GMT$
[[nodiscard]] std::optional<std::int64_t> parse_rfc1123(std::string_view date) {
  // Wdy, DD Mon YYYY HH:MM:SS GMT
  if (date.size() != 29) {
    // allow variable weekday length? Django uses \w{3}
    // "Sun, 06 Nov 1994 08:49:37 GMT" = 29
  }
  std::size_t i = 0;
  // weekday 3 + ", "
  if (date.size() < 5 || date[3] != ',' || date[4] != ' ') {
    return std::nullopt;
  }
  i = 5;
  int day = 0, year = 0, hour = 0, min = 0, sec = 0;
  if (!parse_n_digits(date, i, 2, day) || i >= date.size() || date[i++] != ' ') {
    return std::nullopt;
  }
  if (i + 3 > date.size()) {
    return std::nullopt;
  }
  int month = month_index(date.substr(i, 3));
  if (month < 0) {
    return std::nullopt;
  }
  i += 3;
  if (i >= date.size() || date[i++] != ' ') {
    return std::nullopt;
  }
  if (!parse_n_digits(date, i, 4, year) || i >= date.size() || date[i++] != ' ') {
    return std::nullopt;
  }
  if (!parse_time_hms(date, i, hour, min, sec)) {
    return std::nullopt;
  }
  if (date.substr(i) != " GMT") {
    return std::nullopt;
  }
  return to_unix(year, month, day, hour, min, sec);
}

// RFC 850: Sunday, 06-Nov-94 08:49:37 GMT
// ^\w{6,9}, DD-Mon-YY HH:MM:SS GMT$
[[nodiscard]] std::optional<std::int64_t> parse_rfc850(std::string_view date,
                                                       int current_year) {
  const std::size_t comma = date.find(',');
  if (comma == std::string_view::npos || comma < 6 || comma > 9) {
    return std::nullopt;
  }
  if (comma + 1 >= date.size() || date[comma + 1] != ' ') {
    return std::nullopt;
  }
  std::size_t i = comma + 2;
  int day = 0, year = 0, hour = 0, min = 0, sec = 0;
  if (!parse_n_digits(date, i, 2, day) || i >= date.size() || date[i++] != '-') {
    return std::nullopt;
  }
  if (i + 3 > date.size()) {
    return std::nullopt;
  }
  int month = month_index(date.substr(i, 3));
  if (month < 0) {
    return std::nullopt;
  }
  i += 3;
  if (i >= date.size() || date[i++] != '-') {
    return std::nullopt;
  }
  if (!parse_n_digits(date, i, 2, year) || i >= date.size() || date[i++] != ' ') {
    return std::nullopt;
  }
  // 2-digit year century logic (uses caller-provided current year so tests
  // can mock datetime.now).
  {
    const int current_century = current_year - (current_year % 100);
    if (year - (current_year % 100) > 50) {
      year += current_century - 100;
    } else {
      year += current_century;
    }
  }
  if (!parse_time_hms(date, i, hour, min, sec)) {
    return std::nullopt;
  }
  if (date.substr(i) != " GMT") {
    return std::nullopt;
  }
  return to_unix(year, month, day, hour, min, sec);
}

// asctime: Sun Nov  6 08:49:37 1994
// ^\w{3} Mon [ D]D HH:MM:SS YYYY$
[[nodiscard]] std::optional<std::int64_t> parse_asctime(std::string_view date) {
  if (date.size() < 24) {
    return std::nullopt;
  }
  std::size_t i = 0;
  // Wdy
  if (date.size() < 4 || date[3] != ' ') {
    return std::nullopt;
  }
  i = 4;
  if (i + 3 > date.size()) {
    return std::nullopt;
  }
  int month = month_index(date.substr(i, 3));
  if (month < 0) {
    return std::nullopt;
  }
  i += 3;
  if (i >= date.size() || date[i++] != ' ') {
    return std::nullopt;
  }
  int day = 0, year = 0, hour = 0, min = 0, sec = 0;
  if (!parse_day_flex(date, i, day) || i >= date.size() || date[i++] != ' ') {
    return std::nullopt;
  }
  if (!parse_time_hms(date, i, hour, min, sec) || i >= date.size() || date[i++] != ' ') {
    return std::nullopt;
  }
  if (!parse_n_digits(date, i, 4, year) || i != date.size()) {
    return std::nullopt;
  }
  return to_unix(year, month, day, hour, min, sec);
}

// ETag: (?:W/)?"[^"]*"
[[nodiscard]] bool is_quoted_etag(std::string_view s) {
  std::size_t i = 0;
  if (s.size() >= 2 && s[0] == 'W' && s[1] == '/') {
    i = 2;
  }
  if (i >= s.size() || s[i] != '"') {
    return false;
  }
  ++i;
  while (i < s.size() && s[i] != '"') {
    ++i;
  }
  return i < s.size() && s[i] == '"' && i + 1 == s.size();
}

// host validation: ^([a-z0-9.-]+|\[[a-f0-9]*:[a-f0-9.:]+\])(?::([0-9]+))?$
[[nodiscard]] bool match_host_port(std::string_view host_lower, std::string& domain,
                                   std::string& port) {
  domain.clear();
  port.clear();
  if (host_lower.empty()) {
    return false;
  }
  std::size_t i = 0;
  if (host_lower[0] == '[') {
    // IPv6
    const std::size_t close = host_lower.find(']');
    if (close == std::string_view::npos) {
      return false;
    }
    // inside: [a-f0-9]*:[a-f0-9.:]+  — Django pattern is slightly loose
    const std::string_view inner = host_lower.substr(1, close - 1);
    if (inner.find(':') == std::string_view::npos) {
      return false;
    }
    for (char c : inner) {
      if (!((c >= 'a' && c <= 'f') || (c >= '0' && c <= '9') || c == ':' || c == '.')) {
        return false;
      }
    }
    domain = std::string(host_lower.substr(0, close + 1));
    i = close + 1;
  } else {
    // [a-z0-9.-]+
    std::size_t start = 0;
    while (i < host_lower.size() &&
           ((host_lower[i] >= 'a' && host_lower[i] <= 'z') ||
            (host_lower[i] >= '0' && host_lower[i] <= '9') || host_lower[i] == '.' ||
            host_lower[i] == '-')) {
      ++i;
    }
    if (i == start) {
      return false;
    }
    domain = std::string(host_lower.substr(0, i));
  }
  if (i < host_lower.size()) {
    if (host_lower[i] != ':') {
      return false;
    }
    ++i;
    if (i >= host_lower.size()) {
      return false;
    }
    const std::size_t port_start = i;
    while (i < host_lower.size() && is_digit(host_lower[i])) {
      ++i;
    }
    if (i != host_lower.size() || i == port_start) {
      return false;
    }
    port = std::string(host_lower.substr(port_start));
  }
  // strip trailing dot from domain
  if (!domain.empty() && domain.back() == '.') {
    domain.pop_back();
  }
  return true;
}

[[nodiscard]] int b64_value(char c) noexcept {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 26;
  }
  if (c >= '0' && c <= '9') {
    return c - '0' + 52;
  }
  if (c == '-' || c == '+') {
    return 62;  // urlsafe uses -
  }
  if (c == '_' || c == '/') {
    return 63;
  }
  return -1;
}

}  // namespace

std::optional<std::int64_t> parse_http_date(std::string_view date, int current_year) {
  if (auto t = parse_rfc1123(date)) {
    return t;
  }
  if (auto t = parse_rfc850(date, current_year)) {
    return t;
  }
  if (auto t = parse_asctime(date)) {
    return t;
  }
  return std::nullopt;
}

std::string http_date(std::optional<double> epoch_seconds) {
  std::time_t t;
  if (epoch_seconds.has_value()) {
    t = static_cast<std::time_t>(std::llround(*epoch_seconds));
  } else {
    t = std::time(nullptr);
  }
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  // Wdy, DD Mon YYYY HH:MM:SS GMT
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%s, %02d %s %04d %02d:%02d:%02d GMT",
                kWdays[static_cast<std::size_t>(tm.tm_wday)].data(), tm.tm_mday,
                kMonNames[static_cast<std::size_t>(tm.tm_mon)].data(),
                tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
  return std::string(buf);
}

std::optional<std::uint64_t> base36_to_int(std::string_view s) {
  if (s.size() > 13) {
    return std::nullopt;  // signal "too large" — Python raises ValueError
  }
  if (s.empty()) {
    return std::nullopt;
  }
  std::uint64_t v = 0;
  for (char c : s) {
    int d;
    if (c >= '0' && c <= '9') {
      d = c - '0';
    } else if (c >= 'a' && c <= 'z') {
      d = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'Z') {
      d = c - 'A' + 10;
    } else {
      return std::nullopt;
    }
    v = v * 36 + static_cast<std::uint64_t>(d);
  }
  return v;
}

std::string int_to_base36(std::uint64_t i) {
  static constexpr char kChars[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  if (i < 36) {
    return std::string(1, kChars[i]);
  }
  std::string b36;
  while (i != 0) {
    b36.insert(b36.begin(), kChars[i % 36]);
    i /= 36;
  }
  return b36;
}

std::vector<std::string> parse_etags(std::string_view etag_str) {
  // strip and check *
  std::size_t a = 0;
  std::size_t b = etag_str.size();
  while (a < b && std::isspace(static_cast<unsigned char>(etag_str[a]))) {
    ++a;
  }
  while (b > a && std::isspace(static_cast<unsigned char>(etag_str[b - 1]))) {
    --b;
  }
  if (etag_str.substr(a, b - a) == "*") {
    return {"*"};
  }
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= etag_str.size()) {
    std::size_t comma = etag_str.find(',', start);
    std::string_view part = (comma == std::string_view::npos)
                                ? etag_str.substr(start)
                                : etag_str.substr(start, comma - start);
    // strip
    while (!part.empty() &&
           std::isspace(static_cast<unsigned char>(part.front()))) {
      part.remove_prefix(1);
    }
    while (!part.empty() && std::isspace(static_cast<unsigned char>(part.back()))) {
      part.remove_suffix(1);
    }
    if (is_quoted_etag(part)) {
      out.emplace_back(part);
    }
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  return out;
}

std::string quote_etag(std::string_view etag_str) {
  if (is_quoted_etag(etag_str)) {
    return std::string(etag_str);
  }
  std::string out = "\"";
  out.append(etag_str);
  out += '"';
  return out;
}

bool is_same_domain(std::string_view host, std::string_view pattern) {
  if (pattern.empty()) {
    return false;
  }
  const std::string pat = ascii_lower(pattern);
  if (pat[0] == '.') {
    const bool ends =
        host.size() >= pat.size() &&
        host.compare(host.size() - pat.size(), pat.size(), pat) == 0;
    return ends || host == std::string_view(pat).substr(1);
  }
  return pat == host;
}

std::pair<std::string, std::string> split_domain_port(std::string_view host) {
  const std::string lower = ascii_lower(host);
  std::string domain;
  std::string port;
  if (match_host_port(lower, domain, port)) {
    return {domain, port};
  }
  return {"", ""};
}

bool validate_host(std::string_view host,
                   const std::vector<std::string>& allowed_hosts) {
  for (const auto& pattern : allowed_hosts) {
    if (pattern == "*" || is_same_domain(host, pattern)) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> content_disposition_header(
    bool as_attachment, std::optional<std::string_view> filename) {
  if (filename.has_value() && !filename->empty()) {
    const std::string_view fn = *filename;
    const char* disposition = as_attachment ? "attachment" : "inline";
    bool is_ascii = true;
    bool quotable = true;
    for (unsigned char c : fn) {
      if (c >= 0x80) {
        is_ascii = false;
        break;
      }
      // quotable: tab, space, 0x21-0x7e
      if (!(c == '\t' || c == ' ' || (c >= 0x21 && c <= 0x7e))) {
        quotable = false;
      }
    }
    std::string out = disposition;
    out += "; ";
    if (is_ascii && quotable) {
      out += "filename=\"";
      for (char c : fn) {
        if (c == '\\' || c == '"') {
          out += '\\';
        }
        out += c;
      }
      out += '"';
    } else {
      out += "filename*=utf-8''";
      out += url_quote(fn, "");
    }
    return out;
  }
  if (as_attachment) {
    return std::string("attachment");
  }
  return std::nullopt;
}

std::string urlsafe_base64_encode(std::string_view data) {
  static constexpr char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  std::size_t i = 0;
  while (i + 2 < data.size()) {
    const unsigned n = (static_cast<unsigned char>(data[i]) << 16) |
                       (static_cast<unsigned char>(data[i + 1]) << 8) |
                       static_cast<unsigned char>(data[i + 2]);
    out += kTable[(n >> 18) & 63];
    out += kTable[(n >> 12) & 63];
    out += kTable[(n >> 6) & 63];
    out += kTable[n & 63];
    i += 3;
  }
  if (i + 1 == data.size()) {
    const unsigned n = static_cast<unsigned char>(data[i]) << 16;
    out += kTable[(n >> 18) & 63];
    out += kTable[(n >> 12) & 63];
    // strip padding
  } else if (i + 2 == data.size()) {
    const unsigned n = (static_cast<unsigned char>(data[i]) << 16) |
                       (static_cast<unsigned char>(data[i + 1]) << 8);
    out += kTable[(n >> 18) & 63];
    out += kTable[(n >> 12) & 63];
    out += kTable[(n >> 6) & 63];
  }
  return out;
}

std::optional<std::string> urlsafe_base64_decode(std::string_view s) {
  // pad to multiple of 4
  std::string padded(s);
  while (padded.size() % 4 != 0) {
    padded += '=';
  }
  std::string out;
  out.reserve(padded.size() / 4 * 3);
  for (std::size_t i = 0; i < padded.size(); i += 4) {
    int v[4];
    int pad = 0;
    for (int j = 0; j < 4; ++j) {
      const char c = padded[i + static_cast<std::size_t>(j)];
      if (c == '=') {
        v[j] = 0;
        ++pad;
      } else {
        v[j] = b64_value(c);
        if (v[j] < 0) {
          return std::nullopt;
        }
      }
    }
    const unsigned n = (static_cast<unsigned>(v[0]) << 18) |
                       (static_cast<unsigned>(v[1]) << 12) |
                       (static_cast<unsigned>(v[2]) << 6) |
                       static_cast<unsigned>(v[3]);
    out += static_cast<char>((n >> 16) & 0xFF);
    if (pad < 2) {
      out += static_cast<char>((n >> 8) & 0xFF);
    }
    if (pad < 1) {
      out += static_cast<char>(n & 0xFF);
    }
  }
  return out;
}

}  // namespace django::native
