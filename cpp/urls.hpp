// URL route matching / converters (django.urls.path defaults).
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace django::native {

enum class ConverterKind {
  Int,
  Str,
  Slug,
  Path,
  Uuid,
};

struct RoutePart {
  bool is_literal = false;
  // Literal text, or converter parameter name.
  std::string text;
  ConverterKind kind = ConverterKind::Str;
};

struct CompiledRoute {
  std::vector<RoutePart> parts;
  bool is_endpoint = false;
};

// Compile a django.urls.path() route that only uses default converters.
// Returns nullopt if the route uses an unknown/custom converter name.
[[nodiscard]] std::optional<CompiledRoute> compile_route(std::string_view route,
                                                         bool is_endpoint);

// Match path against a compiled route (from start, like ^regex).
// On success: remaining path suffix + list of (param_name, captured_string).
// Captures are raw strings (caller runs converter.to_python).
struct RouteMatch {
  std::string remaining;
  std::vector<std::pair<std::string, std::string>> kwargs;
};

[[nodiscard]] std::optional<RouteMatch> match_route(const CompiledRoute& route,
                                                    std::string_view path);

// Converter helpers (match django.urls.converters).
[[nodiscard]] long long converter_int_to_python(std::string_view value);
[[nodiscard]] std::string converter_int_to_url(long long value);

// Returns canonical lowercase UUID string; throws std::invalid_argument if bad.
[[nodiscard]] std::string converter_uuid_to_python(std::string_view value);
[[nodiscard]] std::string converter_uuid_to_url(std::string_view value);

// str / slug / path converters (identity with optional validation).
[[nodiscard]] std::string converter_str_to_python(std::string_view value);
[[nodiscard]] std::string converter_str_to_url(std::string_view value);
// Slug: must match [-a-zA-Z0-9_]+ (non-empty). Throws if invalid.
[[nodiscard]] std::string converter_slug_to_python(std::string_view value);
[[nodiscard]] std::string converter_slug_to_url(std::string_view value);
// Path: non-empty (regex `.+`). Throws if empty.
[[nodiscard]] std::string converter_path_to_python(std::string_view value);
[[nodiscard]] std::string converter_path_to_url(std::string_view value);

// Reverse: quote(decoded, safe=RFC3986_SUBDELIMS + "/~:@") then escape // prefix.
[[nodiscard]] std::string reverse_quote(std::string_view decoded);

}  // namespace django::native
