#include "cookie.hpp"

#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace django::native {
namespace {

[[nodiscard]] std::string_view strip(std::string_view s) {
  while (!s.empty() &&
         (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' ||
          s.front() == '\r' || s.front() == '\f' || s.front() == '\v')) {
    s.remove_prefix(1);
  }
  while (!s.empty() &&
         (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r' ||
          s.back() == '\f' || s.back() == '\v')) {
    s.remove_suffix(1);
  }
  return s;
}

}  // namespace

std::string cookie_unquote(std::string_view str) {
  // If there aren't any doublequotes, no special decoding (RFC 2109).
  if (str.size() < 2 || str.front() != '"' || str.back() != '"') {
    return std::string(str);
  }
  // Remove surrounding quotes and process \ooo / \X escapes.
  std::string_view inner = str.substr(1, str.size() - 2);
  std::string out;
  out.reserve(inner.size());
  for (std::size_t i = 0; i < inner.size(); ++i) {
    if (inner[i] != '\\' || i + 1 >= inner.size()) {
      out += inner[i];
      continue;
    }
    // \\(?:([0-3][0-7][0-7])|(.))
    const char c1 = inner[i + 1];
    if (i + 3 < inner.size() && c1 >= '0' && c1 <= '3' && inner[i + 2] >= '0' &&
        inner[i + 2] <= '7' && inner[i + 3] >= '0' && inner[i + 3] <= '7') {
      const int v = (c1 - '0') * 64 + (inner[i + 2] - '0') * 8 + (inner[i + 3] - '0');
      out += static_cast<char>(v);
      i += 3;
    } else {
      out += c1;
      i += 1;
    }
  }
  return out;
}

std::vector<std::pair<std::string, std::string>> parse_cookie(std::string_view cookie) {
  std::vector<std::pair<std::string, std::string>> items;
  std::size_t start = 0;
  while (start <= cookie.size()) {
    std::size_t semi = cookie.find(';', start);
    std::string_view chunk =
        (semi == std::string_view::npos) ? cookie.substr(start)
                                         : cookie.substr(start, semi - start);
    if (semi == std::string_view::npos) {
      start = cookie.size() + 1;
    } else {
      start = semi + 1;
    }

    std::string_view key_sv;
    std::string_view val_sv;
    const std::size_t eq = chunk.find('=');
    if (eq != std::string_view::npos) {
      key_sv = chunk.substr(0, eq);
      val_sv = chunk.substr(eq + 1);
    } else {
      key_sv = "";
      val_sv = chunk;
    }
    key_sv = strip(key_sv);
    val_sv = strip(val_sv);
    if (key_sv.empty() && val_sv.empty()) {
      continue;
    }
    items.emplace_back(std::string(key_sv), cookie_unquote(val_sv));
  }
  return items;
}

}  // namespace django::native
