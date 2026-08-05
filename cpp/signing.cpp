#include "signing.hpp"

#include <climits>
#include <cstdint>
#include <string>
#include <string_view>

namespace django::native {
namespace {

constexpr char kB62[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

[[nodiscard]] int b62_index(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'A' && c <= 'Z') {
    return 10 + (c - 'A');
  }
  if (c >= 'a' && c <= 'z') {
    return 36 + (c - 'a');
  }
  return -1;
}

constexpr char kB64Url[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

[[nodiscard]] int b64_val(char c) noexcept {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }
  if (c >= 'a' && c <= 'z') {
    return 26 + (c - 'a');
  }
  if (c >= '0' && c <= '9') {
    return 52 + (c - '0');
  }
  if (c == '-') {
    return 62;
  }
  if (c == '_') {
    return 63;
  }
  return -1;
}

}  // namespace

std::string b62_encode(std::int64_t value) {
  if (value == 0) {
    return "0";
  }
  std::string sign;
  std::uint64_t s;
  if (value < 0) {
    sign = "-";
    // careful with INT64_MIN
    s = static_cast<std::uint64_t>(-(value + 1)) + 1;
  } else {
    s = static_cast<std::uint64_t>(value);
  }
  std::string encoded;
  while (s > 0) {
    encoded.insert(encoded.begin(), kB62[s % 62]);
    s /= 62;
  }
  return sign + encoded;
}

std::optional<std::int64_t> b62_decode(std::string_view s) {
  if (s.empty()) {
    return std::nullopt;
  }
  if (s == "0") {
    return 0;
  }
  int sign = 1;
  std::size_t i = 0;
  if (s[0] == '-') {
    sign = -1;
    i = 1;
    if (i >= s.size()) {
      return std::nullopt;
    }
  }
  std::int64_t decoded = 0;
  for (; i < s.size(); ++i) {
    int d = b62_index(s[i]);
    if (d < 0) {
      return std::nullopt;
    }
    // overflow check
    if (decoded > (INT64_MAX - d) / 62) {
      return std::nullopt;
    }
    decoded = decoded * 62 + d;
  }
  return sign * decoded;
}

std::string b64_encode(std::string_view data) {
  // urlsafe_b64encode without padding
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  std::size_t i = 0;
  while (i + 2 < data.size()) {
    const unsigned n = (static_cast<unsigned char>(data[i]) << 16) |
                       (static_cast<unsigned char>(data[i + 1]) << 8) |
                       static_cast<unsigned char>(data[i + 2]);
    out += kB64Url[(n >> 18) & 63];
    out += kB64Url[(n >> 12) & 63];
    out += kB64Url[(n >> 6) & 63];
    out += kB64Url[n & 63];
    i += 3;
  }
  if (i + 1 == data.size()) {
    const unsigned n = static_cast<unsigned char>(data[i]) << 16;
    out += kB64Url[(n >> 18) & 63];
    out += kB64Url[(n >> 12) & 63];
  } else if (i + 2 == data.size()) {
    const unsigned n = (static_cast<unsigned char>(data[i]) << 16) |
                       (static_cast<unsigned char>(data[i + 1]) << 8);
    out += kB64Url[(n >> 18) & 63];
    out += kB64Url[(n >> 12) & 63];
    out += kB64Url[(n >> 6) & 63];
  }
  return out;
}

std::optional<std::string> b64_decode(std::string_view data) {
  // pad to multiple of 4
  std::string s(data);
  while (s.size() % 4 != 0) {
    s += '=';
  }
  std::string out;
  out.reserve(s.size() / 4 * 3);
  for (std::size_t i = 0; i < s.size(); i += 4) {
    int a = s[i] == '=' ? 0 : b64_val(s[i]);
    int b = s[i + 1] == '=' ? 0 : b64_val(s[i + 1]);
    int c = s[i + 2] == '=' ? 0 : b64_val(s[i + 2]);
    int d = s[i + 3] == '=' ? 0 : b64_val(s[i + 3]);
    if (a < 0 || b < 0 || (s[i + 2] != '=' && c < 0) ||
        (s[i + 3] != '=' && d < 0)) {
      return std::nullopt;
    }
    const unsigned n = (a << 18) | (b << 12) | (c << 6) | d;
    out += static_cast<char>((n >> 16) & 0xFF);
    if (s[i + 2] != '=') {
      out += static_cast<char>((n >> 8) & 0xFF);
    }
    if (s[i + 3] != '=') {
      out += static_cast<char>(n & 0xFF);
    }
  }
  return out;
}

bool constant_time_compare(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) {
    // still walk to avoid leaking length via early exit on content? Django
    // uses secrets.compare_digest which requires equal length for True.
    // For unequal lengths return false without comparing content timing of
    // length is unavoidable.
    return false;
  }
  unsigned char result = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    result |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
  }
  return result == 0;
}

bool signer_sep_unsafe(std::string_view sep) noexcept {
  if (sep.empty()) {
    return true;
  }
  for (char c : sep) {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '-' || c == '_' || c == '=')) {
      return false;  // has a safe (non-urlsafe-base64) char → sep is OK
    }
  }
  return true;  // only urlsafe chars → unsafe as separator
}

}  // namespace django::native
