#include "signing.hpp"

#include "crypto.hpp"

#include <climits>
#include <cstdint>
#include <string>
#include <string_view>

#include <zlib.h>

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

namespace {

std::optional<std::string> zlib_decompress(std::string_view in) {
  if (in.empty()) {
    return std::string{};
  }
  z_stream strm{};
  if (inflateInit(&strm) != Z_OK) {
    return std::nullopt;
  }
  strm.next_in =
      reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
  strm.avail_in = static_cast<uInt>(in.size());
  std::string out;
  out.resize(in.size() * 4 + 64);
  int ret = Z_OK;
  while (ret == Z_OK) {
    if (strm.total_out >= out.size()) {
      out.resize(out.size() * 2 + 64);
    }
    strm.next_out = reinterpret_cast<Bytef*>(out.data() + strm.total_out);
    strm.avail_out = static_cast<uInt>(out.size() - strm.total_out);
    ret = inflate(&strm, Z_NO_FLUSH);
  }
  const uLong total = strm.total_out;
  inflateEnd(&strm);
  if (ret != Z_STREAM_END) {
    return std::nullopt;
  }
  out.resize(static_cast<std::size_t>(total));
  return out;
}

}  // namespace

std::optional<std::string> signing_base64_hmac(std::string_view salt,
                                              std::string_view value,
                                              std::string_view key,
                                              std::string_view algorithm) {
  auto algo = hash_algo_from_name(algorithm);
  if (!algo) {
    return std::nullopt;
  }
  try {
    std::string dig = salted_hmac_digest(*algo, salt, key, value);
    return b64_encode(dig);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::string> signing_unsign_object_bytes(
    std::string_view signed_value, std::string_view salt,
    std::string_view secret, std::string_view algorithm, std::string_view sep,
    double max_age_seconds, double now_unix) {
  if (sep.empty() || signed_value.empty()) {
    return std::nullopt;
  }
  // value + sep + sig  (rsplit)
  const auto sig_pos = signed_value.rfind(sep);
  if (sig_pos == std::string_view::npos || sig_pos == 0) {
    return std::nullopt;
  }
  const std::string_view value = signed_value.substr(0, sig_pos);
  const std::string_view sig =
      signed_value.substr(sig_pos + sep.size());
  if (sig.empty()) {
    return std::nullopt;
  }

  // base64_hmac(salt + "signer", value, secret, algorithm)
  std::string key_salt;
  key_salt.reserve(salt.size() + 6);
  key_salt.append(salt);
  key_salt.append("signer");
  auto expected = signing_base64_hmac(key_salt, value, secret, algorithm);
  if (!expected || !constant_time_compare(sig, *expected)) {
    return std::nullopt;
  }

  // TimestampSigner: value is base64d + sep + timestamp
  const auto ts_pos = value.rfind(sep);
  if (ts_pos == std::string_view::npos) {
    return std::nullopt;
  }
  std::string_view base64d = value.substr(0, ts_pos);
  const std::string_view ts_str = value.substr(ts_pos + sep.size());
  if (max_age_seconds >= 0.0) {
    auto ts = b62_decode(ts_str);
    if (!ts) {
      return std::nullopt;
    }
    const double now = now_unix > 0.0 ? now_unix : 0.0;
    // Caller should pass now; if 0, skip age (treat as no check).
    if (now > 0.0) {
      const double age = now - static_cast<double>(*ts);
      if (age > max_age_seconds) {
        return std::nullopt;
      }
    }
  }

  bool decompress = !base64d.empty() && base64d.front() == '.';
  if (decompress) {
    base64d = base64d.substr(1);
  }
  auto data = b64_decode(base64d);
  if (!data) {
    return std::nullopt;
  }
  if (decompress) {
    return zlib_decompress(*data);
  }
  return data;
}

}  // namespace django::native
