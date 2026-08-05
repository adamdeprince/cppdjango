// Template expression parsing: smart_split, Variable, FilterExpression.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace django::native {

// --- smart_split ----------------------------------------------------------
[[nodiscard]] std::vector<std::string> smart_split(std::string_view text);

// --- unescape_string_literal ----------------------------------------------
// Returns unquoted string, or nullopt if not a string literal.
[[nodiscard]] std::optional<std::string> unescape_string_literal(std::string_view s);

// --- Variable token classification ----------------------------------------
enum class VariableKind {
  IntLiteral,
  FloatLiteral,
  StringLiteral,
  Lookup,
  Error,
};

struct ParsedVariable {
  VariableKind kind = VariableKind::Error;
  // For literals:
  long long int_value = 0;
  double float_value = 0.0;
  std::string string_value;
  // For lookups:
  std::vector<std::string> lookups;
  bool translate = false;
  // For errors:
  std::string error;  // "underscore" | "invalid_char" | "type"
  std::string error_detail;
};

[[nodiscard]] ParsedVariable parse_variable(std::string_view var);

// --- FilterExpression token parse -----------------------------------------
// One match from filter_re (constant/var at start, or filter with optional arg).
enum class FilterMatchKind {
  Constant,   // quoted / _(quoted) constant at expression start
  Var,        // variable name at expression start
  Filter,     // |filter_name[:arg]
};

struct FilterArg {
  bool is_var = false;  // true → variable arg; false → constant (already resolved
                        // string/number stored in constant_token for Python to
                        // Variable()-resolve)
  std::string token;    // raw token for Variable() or constant string token
};

struct FilterMatch {
  FilterMatchKind kind = FilterMatchKind::Var;
  std::string token;          // constant/var token, or filter name
  std::optional<FilterArg> arg;  // only for Filter with argument
  std::size_t start = 0;
  std::size_t end = 0;
};

// Parse a full filter expression token (e.g. 'article.section|upper|default:"x"').
// Throws std::invalid_argument with Django-like messages on syntax errors.
// Returns ordered matches covering the full string without gaps.
[[nodiscard]] std::vector<FilterMatch> parse_filter_expression(std::string_view token);

// Nested dict-only lookup: context must be a dict-like mapping of strings.
// Returns nullopt if any step fails (caller falls back to Python).
// Implemented in bindings with Python objects — declared here as free logic
// on flat string maps is insufficient; see module.cpp.

}  // namespace django::native
