#include "urls.hpp"

#include "filters.hpp"  // url_quote, escape_leading_slashes

#include <cctype>
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace django::native {
namespace {

[[nodiscard]] bool is_digit(char c) noexcept {
  return c >= '0' && c <= '9';
}

[[nodiscard]] bool is_hex_lower(char c) noexcept {
  return is_digit(c) || (c >= 'a' && c <= 'f');
}

[[nodiscard]] bool is_slug_char(char c) noexcept {
  return is_digit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' ||
         c == '_';
}

[[nodiscard]] std::optional<ConverterKind> kind_from_name(std::string_view name) {
  if (name == "int") {
    return ConverterKind::Int;
  }
  if (name == "str") {
    return ConverterKind::Str;
  }
  if (name == "slug") {
    return ConverterKind::Slug;
  }
  if (name == "path") {
    return ConverterKind::Path;
  }
  if (name == "uuid") {
    return ConverterKind::Uuid;
  }
  return std::nullopt;
}

[[nodiscard]] bool match_int_at(std::string_view path, std::size_t i, std::size_t& end,
                                std::string& capture) {
  if (i >= path.size() || !is_digit(path[i])) {
    return false;
  }
  end = i;
  while (end < path.size() && is_digit(path[end])) {
    ++end;
  }
  capture = std::string(path.substr(i, end - i));
  return true;
}

[[nodiscard]] bool match_str_at(std::string_view path, std::size_t i, std::size_t& end,
                                std::string& capture) {
  if (i >= path.size() || path[i] == '/') {
    return false;
  }
  end = i;
  while (end < path.size() && path[end] != '/') {
    ++end;
  }
  capture = std::string(path.substr(i, end - i));
  return true;
}

[[nodiscard]] bool match_slug_at(std::string_view path, std::size_t i, std::size_t& end,
                                 std::string& capture) {
  if (i >= path.size() || !is_slug_char(path[i])) {
    return false;
  }
  end = i;
  while (end < path.size() && is_slug_char(path[end])) {
    ++end;
  }
  capture = std::string(path.substr(i, end - i));
  return true;
}

[[nodiscard]] bool match_uuid_at(std::string_view path, std::size_t i, std::size_t& end,
                                 std::string& capture) {
  static constexpr int groups[] = {8, 4, 4, 4, 12};
  end = i;
  for (int g = 0; g < 5; ++g) {
    if (g > 0) {
      if (end >= path.size() || path[end] != '-') {
        return false;
      }
      ++end;
    }
    for (int n = 0; n < groups[g]; ++n) {
      if (end >= path.size() || !is_hex_lower(path[end])) {
        return false;
      }
      ++end;
    }
  }
  capture = std::string(path.substr(i, end - i));
  return true;
}

[[nodiscard]] bool is_identifier_start(char c) noexcept {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

[[nodiscard]] bool is_identifier_cont(char c) noexcept {
  return is_identifier_start(c) || is_digit(c);
}

[[nodiscard]] bool looks_like_identifier(std::string_view s) {
  if (s.empty() || !is_identifier_start(s[0])) {
    return false;
  }
  for (char c : s) {
    if (!is_identifier_cont(c)) {
      return false;
    }
  }
  return true;
}

// Recursive match so <path:...> (regex `.+`) can backtrack like Python re.
bool match_from(const CompiledRoute& route, std::size_t part_idx, std::string_view path,
                std::size_t pos, RouteMatch& out) {
  if (part_idx == route.parts.size()) {
    if (route.is_endpoint) {
      if (pos != path.size()) {
        return false;
      }
      out.remaining.clear();
    } else {
      out.remaining = std::string(path.substr(pos));
    }
    return true;
  }

  const RoutePart& part = route.parts[part_idx];
  if (part.is_literal) {
    if (path.substr(pos, part.text.size()) != part.text) {
      return false;
    }
    return match_from(route, part_idx + 1, path, pos + part.text.size(), out);
  }

  if (part.kind == ConverterKind::Path) {
    // Greedy `.+` with backtracking: try longest capture first (at least 1).
    if (pos >= path.size()) {
      return false;
    }
    for (std::size_t end = path.size(); end > pos; --end) {
      const std::size_t kwargs_size = out.kwargs.size();
      out.kwargs.emplace_back(part.text, std::string(path.substr(pos, end - pos)));
      if (match_from(route, part_idx + 1, path, end, out)) {
        return true;
      }
      out.kwargs.resize(kwargs_size);
    }
    return false;
  }

  std::size_t end = pos;
  std::string capture;
  bool ok = false;
  switch (part.kind) {
    case ConverterKind::Int:
      ok = match_int_at(path, pos, end, capture);
      break;
    case ConverterKind::Str:
      ok = match_str_at(path, pos, end, capture);
      break;
    case ConverterKind::Slug:
      ok = match_slug_at(path, pos, end, capture);
      break;
    case ConverterKind::Uuid:
      ok = match_uuid_at(path, pos, end, capture);
      break;
    case ConverterKind::Path:
      break;  // handled above
  }
  if (!ok) {
    return false;
  }
  const std::size_t kwargs_size = out.kwargs.size();
  out.kwargs.emplace_back(part.text, std::move(capture));
  if (match_from(route, part_idx + 1, path, end, out)) {
    return true;
  }
  out.kwargs.resize(kwargs_size);
  return false;
}

}  // namespace

std::optional<CompiledRoute> compile_route(std::string_view route, bool is_endpoint) {
  CompiledRoute compiled;
  compiled.is_endpoint = is_endpoint;

  std::size_t i = 0;
  while (i < route.size()) {
    if (route[i] == '<') {
      const std::size_t close = route.find('>', i + 1);
      if (close == std::string_view::npos) {
        return std::nullopt;
      }
      const std::string_view inner = route.substr(i + 1, close - i - 1);
      for (char c : inner) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
          return std::nullopt;
        }
      }
      std::string_view conv_name = "str";
      std::string_view param;
      const std::size_t colon = inner.find(':');
      if (colon == std::string_view::npos) {
        param = inner;
      } else {
        conv_name = inner.substr(0, colon);
        param = inner.substr(colon + 1);
      }
      if (param.empty() || !looks_like_identifier(param)) {
        return std::nullopt;
      }
      const auto kind = kind_from_name(conv_name);
      if (!kind.has_value()) {
        return std::nullopt;
      }
      RoutePart part;
      part.is_literal = false;
      part.text = std::string(param);
      part.kind = *kind;
      compiled.parts.push_back(std::move(part));
      i = close + 1;
    } else {
      const std::size_t next = route.find('<', i);
      const std::size_t end = (next == std::string_view::npos) ? route.size() : next;
      RoutePart part;
      part.is_literal = true;
      part.text = std::string(route.substr(i, end - i));
      compiled.parts.push_back(std::move(part));
      i = end;
    }
  }
  return compiled;
}

std::optional<RouteMatch> match_route(const CompiledRoute& route,
                                      std::string_view path) {
  RouteMatch result;
  result.kwargs.reserve(route.parts.size());
  if (!match_from(route, 0, path, 0, result)) {
    return std::nullopt;
  }
  return result;
}

long long converter_int_to_python(std::string_view value) {
  if (value.empty()) {
    throw std::invalid_argument("empty int");
  }
  long long result = 0;
  const char* begin = value.data();
  const char* end = value.data() + value.size();
  auto [ptr, ec] = std::from_chars(begin, end, result);
  if (ec != std::errc{} || ptr != end) {
    throw std::invalid_argument("invalid int");
  }
  return result;
}

std::string converter_int_to_url(long long value) { return std::to_string(value); }

std::string converter_uuid_to_python(std::string_view value) {
  std::string s(value);
  for (char& c : s) {
    if (c >= 'A' && c <= 'F') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  std::size_t end = 0;
  std::string capture;
  if (!match_uuid_at(s, 0, end, capture) || end != s.size()) {
    throw std::invalid_argument("invalid uuid");
  }
  return capture;
}

std::string converter_uuid_to_url(std::string_view value) {
  // Canonical form is lowercase hex (uuid.UUID.__str__).
  std::string s(value);
  for (char& c : s) {
    if (c >= 'A' && c <= 'F') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return s;
}

std::string converter_str_to_python(std::string_view value) {
  return std::string(value);
}

std::string converter_str_to_url(std::string_view value) {
  return std::string(value);
}

std::string converter_slug_to_python(std::string_view value) {
  if (value.empty()) {
    throw std::invalid_argument("empty slug");
  }
  for (char c : value) {
    if (!is_slug_char(c)) {
      throw std::invalid_argument("invalid slug");
    }
  }
  return std::string(value);
}

std::string converter_slug_to_url(std::string_view value) {
  // Same charset check as match (rejects non-slug for reverse).
  return converter_slug_to_python(value);
}

std::string converter_path_to_python(std::string_view value) {
  if (value.empty()) {
    throw std::invalid_argument("empty path");
  }
  return std::string(value);
}

std::string converter_path_to_url(std::string_view value) {
  return converter_path_to_python(value);
}

std::string reverse_quote(std::string_view decoded) {
  // RFC3986_SUBDELIMS + "/~:@"  (django.urls.resolvers reverse)
  constexpr std::string_view kSafe = "!$&'()*+,;=/~:@";
  return escape_leading_slashes(url_quote(decoded, kSafe));
}

}  // namespace django::native
