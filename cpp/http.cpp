#include "http.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace django::native {
namespace {

[[nodiscard]] int hex_value(char c) noexcept {
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

// Percent-decode + treat '+' as space. Output is raw bytes as a std::string
// (byte-oriented), matching CPython _unquote_impl for ASCII/percent input.
// Invalid % sequences are left as literal '%' + rest (CPython behaviour).
[[nodiscard]] std::string unquote_to_bytes(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    const char c = in[i];
    if (c == '+') {
      out += ' ';
      continue;
    }
    if (c == '%' && i + 2 < in.size()) {
      const int hi = hex_value(in[i + 1]);
      const int lo = hex_value(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += static_cast<char>((hi << 4) | lo);
        i += 2;
        continue;
      }
    }
    out += c;
  }
  return out;
}

// Decode UTF-8 with errors='replace' (U+FFFD).
[[nodiscard]] std::string utf8_decode_replace(std::string_view bytes) {
  std::string out;
  out.reserve(bytes.size());
  std::size_t i = 0;
  while (i < bytes.size()) {
    const auto b0 = static_cast<unsigned char>(bytes[i]);
    if (b0 < 0x80) {
      out += static_cast<char>(b0);
      ++i;
      continue;
    }

    auto append_fffd = [&]() {
      out += "\xEF\xBF\xBD";  // U+FFFD in UTF-8
    };

    int need = 0;
    char32_t cp = 0;
    if ((b0 & 0xE0) == 0xC0) {
      need = 1;
      cp = b0 & 0x1F;
      if (b0 < 0xC2) {  // overlong
        append_fffd();
        ++i;
        continue;
      }
    } else if ((b0 & 0xF0) == 0xE0) {
      need = 2;
      cp = b0 & 0x0F;
    } else if ((b0 & 0xF8) == 0xF0) {
      need = 3;
      cp = b0 & 0x07;
      if (b0 > 0xF4) {
        append_fffd();
        ++i;
        continue;
      }
    } else {
      append_fffd();
      ++i;
      continue;
    }

    if (i + static_cast<std::size_t>(need) >= bytes.size()) {
      append_fffd();
      break;
    }

    bool ok = true;
    for (int j = 1; j <= need; ++j) {
      const auto bj = static_cast<unsigned char>(bytes[i + static_cast<std::size_t>(j)]);
      if ((bj & 0xC0) != 0x80) {
        ok = false;
        break;
      }
      cp = (cp << 6) | (bj & 0x3F);
    }
    if (!ok || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
      append_fffd();
      ++i;
      continue;
    }
    // Emit UTF-8 for cp (usually same as original valid sequence).
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
    i += static_cast<std::size_t>(need) + 1;
  }
  return out;
}

// unquote_plus for UTF-8 query components.
// Mirrors CPython: if no '%', return after '+' → space only (keep non-ASCII).
[[nodiscard]] std::string unquote_plus_utf8(std::string_view in) {
  bool has_percent = false;
  bool has_plus = false;
  for (char c : in) {
    if (c == '%') {
      has_percent = true;
    } else if (c == '+') {
      has_plus = true;
    }
  }
  if (!has_percent) {
    if (!has_plus) {
      return std::string(in);
    }
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
      out += (c == '+') ? ' ' : c;
    }
    return out;
  }
  // Percent-decode then UTF-8 decode with replace.
  // CPython encodes the str as UTF-8 first, then percent-decodes bytes.
  // Non-ASCII in `in` is already UTF-8 in our std::string from Python.
  const std::string raw = unquote_to_bytes(in);
  return utf8_decode_replace(raw);
}

}  // namespace

std::vector<std::pair<std::string, std::string>> parse_qsl_utf8(
    std::string_view qs, std::optional<std::size_t> max_num_fields) {
  if (max_num_fields.has_value()) {
    std::size_t seps = 0;
    for (char c : qs) {
      if (c == '&') {
        ++seps;
      }
    }
    // CPython: num_fields = 1 + qs.count(separator)
    const std::size_t num_fields = seps + 1;
    if (max_num_fields.value() < num_fields) {
      throw std::invalid_argument("Max number of fields exceeded");
    }
  }

  std::vector<std::pair<std::string, std::string>> result;
  if (qs.empty()) {
    return result;
  }

  result.reserve(8);
  std::size_t start = 0;
  while (start <= qs.size()) {
    std::size_t amp = qs.find('&', start);
    if (amp == std::string_view::npos) {
      amp = qs.size();
    }
    const std::string_view part = qs.substr(start, amp - start);
    // Skip empty segments (strict_parsing=False).
    if (!part.empty()) {
      const std::size_t eq = part.find('=');
      std::string_view name;
      std::string_view value;
      if (eq == std::string_view::npos) {
        name = part;
        value = "";
      } else {
        name = part.substr(0, eq);
        value = part.substr(eq + 1);
      }
      // keep_blank_values=True: always keep (including empty values).
      result.emplace_back(unquote_plus_utf8(name), unquote_plus_utf8(value));
    }
    if (amp == qs.size()) {
      break;
    }
    start = amp + 1;
  }
  return result;
}

}  // namespace django::native
