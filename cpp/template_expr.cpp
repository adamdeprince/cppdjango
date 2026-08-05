#include "template_expr.hpp"

#include <cctype>
#include <charconv>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace django::native {
namespace {

[[nodiscard]] bool is_ws(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// \w for template vars: ASCII alnum/underscore + non-ASCII (UTF-8 lead) approx.
[[nodiscard]] bool is_word_byte(unsigned char c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         c == '_' || c >= 0x80;
}

[[nodiscard]] bool is_var_char(char c) noexcept {
  return is_word_byte(static_cast<unsigned char>(c)) || c == '.' || c == '+' ||
         c == '-';
}

// Match double-quoted string: "([^"\\]|\\.)*"
[[nodiscard]] bool match_dq_string(std::string_view s, std::size_t i, std::size_t& end) {
  if (i >= s.size() || s[i] != '"') {
    return false;
  }
  ++i;
  while (i < s.size()) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      i += 2;
      continue;
    }
    if (s[i] == '"') {
      end = i + 1;
      return true;
    }
    ++i;
  }
  return false;
}

[[nodiscard]] bool match_sq_string(std::string_view s, std::size_t i, std::size_t& end) {
  if (i >= s.size() || s[i] != '\'') {
    return false;
  }
  ++i;
  while (i < s.size()) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      i += 2;
      continue;
    }
    if (s[i] == '\'') {
      end = i + 1;
      return true;
    }
    ++i;
  }
  return false;
}

// constant: _("...") | _('...') | "..." | '...'
[[nodiscard]] bool match_constant(std::string_view s, std::size_t i, std::size_t& end) {
  if (i + 2 < s.size() && s[i] == '_' && s[i + 1] == '(') {
    std::size_t inner_end = 0;
    if (match_dq_string(s, i + 2, inner_end) || match_sq_string(s, i + 2, inner_end)) {
      if (inner_end < s.size() && s[inner_end] == ')') {
        end = inner_end + 1;
        return true;
      }
    }
    return false;
  }
  return match_dq_string(s, i, end) || match_sq_string(s, i, end);
}

[[nodiscard]] bool match_var_token(std::string_view s, std::size_t i, std::size_t& end) {
  if (i >= s.size() || !is_var_char(s[i])) {
    return false;
  }
  // Must start with var char; first char of a "var" in filter_re is [var_chars]+
  // Note: numbers like 5.2e3 are vars at the grammar level.
  end = i;
  while (end < s.size() && is_var_char(s[end])) {
    ++end;
  }
  return end > i;
}

[[nodiscard]] bool match_filter_name(std::string_view s, std::size_t i,
                                     std::size_t& end) {
  // \w+
  if (i >= s.size() || !is_word_byte(static_cast<unsigned char>(s[i]))) {
    return false;
  }
  // first char shouldn't be only non-word for \w — digits ok in Python \w
  end = i;
  while (end < s.size() && is_word_byte(static_cast<unsigned char>(s[end]))) {
    ++end;
  }
  return end > i;
}

}  // namespace

std::vector<std::string> smart_split(std::string_view text) {
  // ((?:[^\s'"]*(?:(?:"(?:[^"\\]|\\.)*" | '(?:[^'\\]|\\.)*')[^\s'"]*)+) | \S+)
  // Alt1 requires at least one complete quoted string; else \S+.
  std::vector<std::string> out;
  std::size_t i = 0;
  const std::size_t n = text.size();
  while (i < n) {
    while (i < n && is_ws(text[i])) {
      ++i;
    }
    if (i >= n) {
      break;
    }

    // --- try alternative 1 ---
    std::size_t j = i;
    std::string bit;
    // [^\s'"]*
    while (j < n && !is_ws(text[j]) && text[j] != '"' && text[j] != '\'') {
      bit += text[j++];
    }
    std::size_t quoted_count = 0;
    while (j < n && (text[j] == '"' || text[j] == '\'')) {
      std::size_t qend = 0;
      const bool ok = (text[j] == '"') ? match_dq_string(text, j, qend)
                                       : match_sq_string(text, j, qend);
      if (!ok) {
        break;
      }
      bit.append(text.substr(j, qend - j));
      j = qend;
      ++quoted_count;
      // [^\s'"]*
      while (j < n && !is_ws(text[j]) && text[j] != '"' && text[j] != '\'') {
        bit += text[j++];
      }
    }
    if (quoted_count > 0) {
      out.push_back(std::move(bit));
      i = j;
      continue;
    }

    // --- alternative 2: \S+ ---
    j = i;
    while (j < n && !is_ws(text[j])) {
      ++j;
    }
    out.emplace_back(text.substr(i, j - i));
    i = j;
  }
  return out;
}

std::optional<std::string> unescape_string_literal(std::string_view s) {
  if (s.size() < 2) {
    return std::nullopt;
  }
  const char q = s.front();
  if ((q != '"' && q != '\'') || s.back() != q) {
    return std::nullopt;
  }
  std::string inner(s.substr(1, s.size() - 2));
  // First unescape quote (backslash + matching quote).
  std::string step1;
  step1.reserve(inner.size());
  for (std::size_t i = 0; i < inner.size(); ++i) {
    if (i + 1 < inner.size() && inner[i] == '\\' && inner[i + 1] == q) {
      step1 += q;
      ++i;
    } else {
      step1 += inner[i];
    }
  }
  // Then unescape doubled backslashes.
  std::string out;
  out.reserve(step1.size());
  for (std::size_t i = 0; i < step1.size(); ++i) {
    if (i + 1 < step1.size() && step1[i] == '\\' && step1[i + 1] == '\\') {
      out += '\\';
      ++i;
    } else {
      out += step1[i];
    }
  }
  return out;
}

ParsedVariable parse_variable(std::string_view var) {
  ParsedVariable result;
  if (var.empty()) {
    result.kind = VariableKind::Error;
    result.error = "empty";
    return result;
  }

  // Try number first (same order as Django).
  try {
    if (var.find('.') != std::string_view::npos ||
        var.find('e') != std::string_view::npos ||
        var.find('E') != std::string_view::npos) {
      // float path — reject trailing lone '.'
      if (var.back() == '.') {
        throw std::invalid_argument("trailing dot");
      }
      // Use from_chars for float if available; else strtod
      std::string tmp(var);
      char* endp = nullptr;
      const double v = std::strtod(tmp.c_str(), &endp);
      if (endp != tmp.c_str() + tmp.size()) {
        throw std::invalid_argument("not float");
      }
      result.kind = VariableKind::FloatLiteral;
      result.float_value = v;
      return result;
    }
    // int (optional leading '+' / '-'); support big integers via string_value.
    long long v = 0;
    const char* begin = var.data();
    const char* end = var.data() + var.size();
    bool negative = false;
    if (begin < end && *begin == '+') {
      ++begin;
    } else if (begin < end && *begin == '-') {
      negative = true;
      ++begin;
    }
    if (begin == end) {
      throw std::invalid_argument("not int");
    }
    for (const char* p = begin; p < end; ++p) {
      if (*p < '0' || *p > '9') {
        throw std::invalid_argument("not int");
      }
    }
    auto [ptr, ec] = std::from_chars(begin, end, v);
    result.kind = VariableKind::IntLiteral;
    if (ec == std::errc::result_out_of_range || ptr != end) {
      // Arbitrary-precision int — Python constructs via int(string_value).
      result.string_value = std::string(var);
      result.int_value = 0;
    } else {
      result.int_value = negative ? -v : v;
    }
    return result;
  } catch (...) {
    // not a number
  }

  std::string_view work = var;
  if (work.size() >= 3 && work.substr(0, 2) == "_(" && work.back() == ')') {
    result.translate = true;
    work = work.substr(2, work.size() - 3);
  }

  if (auto lit = unescape_string_literal(work)) {
    result.kind = VariableKind::StringLiteral;
    result.string_value = std::move(*lit);
    return result;
  }

  // lookup
  if (work.find("._") != std::string_view::npos ||
      (!work.empty() && work.front() == '_')) {
    result.kind = VariableKind::Error;
    result.error = "underscore";
    result.error_detail = std::string(var);
    return result;
  }
  for (char c : std::string{"+-"}) {
    if (work.find(c) != std::string_view::npos) {
      result.kind = VariableKind::Error;
      result.error = "invalid_char";
      result.error_detail = std::string(1, c);
      result.string_value = std::string(var);
      return result;
    }
  }

  result.kind = VariableKind::Lookup;
  // split on '.'
  std::size_t start = 0;
  while (start <= work.size()) {
    std::size_t dot = work.find('.', start);
    if (dot == std::string_view::npos) {
      result.lookups.emplace_back(work.substr(start));
      break;
    }
    result.lookups.emplace_back(work.substr(start, dot - start));
    start = dot + 1;
  }
  return result;
}

std::vector<FilterMatch> parse_filter_expression(std::string_view token) {
  std::vector<FilterMatch> matches;
  std::size_t pos = 0;
  bool have_var = false;

  while (pos < token.size()) {
    FilterMatch m;
    m.start = pos;

    if (!have_var) {
      std::size_t end = 0;
      if (match_constant(token, pos, end)) {
        m.kind = FilterMatchKind::Constant;
        m.token = std::string(token.substr(pos, end - pos));
        m.end = end;
        matches.push_back(std::move(m));
        pos = end;
        have_var = true;
        continue;
      }
      if (match_var_token(token, pos, end)) {
        m.kind = FilterMatchKind::Var;
        m.token = std::string(token.substr(pos, end - pos));
        m.end = end;
        matches.push_back(std::move(m));
        pos = end;
        have_var = true;
        continue;
      }
      throw std::invalid_argument(
          "Could not find variable at start of " + std::string(token));
    }

    // filters: \s*|\s* name ( : arg )?
    std::size_t i = pos;
    while (i < token.size() && is_ws(token[i])) {
      ++i;
    }
    if (i >= token.size()) {
      break;
    }
    if (token[i] != '|') {
      throw std::invalid_argument(
          "Could not parse some characters: " + std::string(token.substr(0, pos)) +
          "|" + std::string(token.substr(pos, i - pos)) + "|" +
          std::string(token.substr(i)));
    }
    ++i;
    while (i < token.size() && is_ws(token[i])) {
      ++i;
    }
    std::size_t name_end = 0;
    if (!match_filter_name(token, i, name_end)) {
      throw std::invalid_argument(
          "Could not parse some characters: " + std::string(token.substr(0, pos)) +
          "|" + std::string(token.substr(pos)) + "|");
    }
    m.kind = FilterMatchKind::Filter;
    m.token = std::string(token.substr(i, name_end - i));
    i = name_end;
    // optional arg
    if (i < token.size() && token[i] == ':') {
      ++i;
      std::size_t arg_end = 0;
      FilterArg arg;
      if (match_constant(token, i, arg_end)) {
        arg.is_var = false;
        arg.token = std::string(token.substr(i, arg_end - i));
        i = arg_end;
      } else if (match_var_token(token, i, arg_end)) {
        arg.is_var = true;
        arg.token = std::string(token.substr(i, arg_end - i));
        i = arg_end;
      } else {
        throw std::invalid_argument("Could not parse filter argument in " +
                                    std::string(token));
      }
      m.arg = std::move(arg);
    }
    m.start = pos;
    m.end = i;
    matches.push_back(std::move(m));
    pos = i;
  }

  if (pos != token.size()) {
    throw std::invalid_argument("Could not parse the remainder: '" +
                                std::string(token.substr(pos)) + "' from '" +
                                std::string(token) + "'");
  }
  if (!have_var) {
    throw std::invalid_argument("Could not find variable at start of " +
                                std::string(token));
  }
  return matches;
}

}  // namespace django::native
