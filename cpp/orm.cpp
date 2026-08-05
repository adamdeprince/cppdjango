#include "orm.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

// isalnum used by accepts_gzip

namespace django::native {
namespace {

[[nodiscard]] std::string ascii_lower(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c >= 'A' && c <= 'Z') {
      out += static_cast<char>(c - 'A' + 'a');
    } else {
      out += c;
    }
  }
  return out;
}

}  // namespace

std::string sql_quote_name(std::string_view name, std::string_view style) {
  const bool backtick = (style == "backtick" || style == "`");
  const char q = backtick ? '`' : '"';
  if (name.size() >= 2 && name.front() == q && name.back() == q) {
    return std::string(name);
  }
  std::string out;
  out.reserve(name.size() + 2);
  out += q;
  out.append(name);
  out += q;
  return out;
}

std::pair<int, int> where_needed_counts(std::string_view connector, int n_children) {
  if (n_children < 0) {
    n_children = 0;
  }
  // AND (and default): need all full, one empty to empty-set
  // OR/XOR: one full to full-set, all empty to empty-set
  if (connector == "AND") {
    return {n_children, 1};
  }
  return {1, n_children};
}

std::string where_combine_sql(std::string_view connector,
                              const std::vector<std::string>& parts, bool negated,
                              bool resolved) {
  if (parts.empty()) {
    return {};
  }
  std::string conn = " ";
  conn.append(connector);
  conn += ' ';
  std::string sql;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) {
      sql += conn;
    }
    sql += parts[i];
  }
  if (negated) {
    return "NOT (" + sql + ")";
  }
  if (parts.size() > 1 || resolved) {
    return "(" + sql + ")";
  }
  return sql;
}

std::string sql_in_placeholders(int n) {
  if (n <= 0) {
    return {};
  }
  std::string out;
  out.reserve(static_cast<std::size_t>(n) * 4);
  for (int i = 0; i < n; ++i) {
    if (i) {
      out += ", ";
    }
    out += "%s";
  }
  return out;
}

std::string sql_isnull_sql(bool negated) {
  return negated ? "IS NOT NULL" : "IS NULL";
}

std::string sql_comparison_rhs(std::string_view lookup_name) {
  if (lookup_name == "exact") {
    return " = %s";
  }
  if (lookup_name == "iexact") {
    return "";  // backend-specific
  }
  if (lookup_name == "gt") {
    return " > %s";
  }
  if (lookup_name == "gte") {
    return " >= %s";
  }
  if (lookup_name == "lt") {
    return " < %s";
  }
  if (lookup_name == "lte") {
    return " <= %s";
  }
  if (lookup_name == "contains" || lookup_name == "icontains") {
    return "";
  }
  return {};
}

bool is_form_empty_string(std::string_view value) noexcept {
  return value.empty();
}

bool field_str_has_changed(std::string_view initial, std::string_view data) noexcept {
  // None already normalized to "" by caller.
  return initial != data;
}

int boolean_field_to_python(std::string_view value) noexcept {
  // BooleanField: 'false'/'0' → False; else bool(value)
  std::string low = ascii_lower(value);
  if (low == "false" || low == "0") {
    return 0;
  }
  return value.empty() ? 0 : 1;
}

int null_boolean_to_python(std::string_view value) noexcept {
  if (value == "True" || value == "true" || value == "1") {
    return 1;
  }
  if (value == "False" || value == "false" || value == "0") {
    return 0;
  }
  return -1;
}

std::vector<std::string> choice_format_values(const std::vector<std::string>& values,
                                              bool allow_none_as_empty) {
  (void)allow_none_as_empty;
  // Values already stringified by Python; pass through (identity for list).
  return values;
}

bool header_key_valid(std::string_view key) noexcept {
  if (key.empty()) {
    return false;
  }
  for (unsigned char c : key) {
    if (c > 127 || c == '\n' || c == '\r') {
      return false;
    }
  }
  return true;
}

bool header_value_no_newlines(std::string_view value) noexcept {
  for (char c : value) {
    if (c == '\n' || c == '\r') {
      return false;
    }
  }
  return true;
}

std::string charset_from_content_type(std::string_view content_type) {
  // ;\\s*charset=(?P<charset>[^\\s;]+)
  auto pos = content_type.find("charset=");
  if (pos == std::string_view::npos) {
    // case-insensitive search
    std::string low = ascii_lower(content_type);
    auto p2 = low.find("charset=");
    if (p2 == std::string::npos) {
      return {};
    }
    pos = p2;
  }
  std::size_t i = pos + 8;  // after charset=
  while (i < content_type.size() &&
         (content_type[i] == ' ' || content_type[i] == '\t')) {
    ++i;
  }
  std::size_t start = i;
  while (i < content_type.size() && content_type[i] != ';' && content_type[i] != ' ' &&
         content_type[i] != '\t') {
    ++i;
  }
  std::string cs(content_type.substr(start, i - start));
  // strip quotes
  if (cs.size() >= 2 && cs.front() == '"' && cs.back() == '"') {
    cs = cs.substr(1, cs.size() - 2);
  }
  return cs;
}

bool path_ends_with_slash(std::string_view path) noexcept {
  return !path.empty() && path.back() == '/';
}

std::string force_append_slash_path(std::string_view full_path) {
  // Append slash before query/fragment if path portion lacks trailing slash.
  std::size_t cut = full_path.find_first_of("?#");
  std::string_view path =
      cut == std::string_view::npos ? full_path : full_path.substr(0, cut);
  std::string_view rest =
      cut == std::string_view::npos ? std::string_view{} : full_path.substr(cut);
  if (!path.empty() && path.back() == '/') {
    return std::string(full_path);
  }
  std::string out;
  out.reserve(full_path.size() + 1);
  out.append(path);
  out += '/';
  out.append(rest);
  return out;
}

std::vector<std::string> serialize_header_lines(
    const std::vector<std::pair<std::string, std::string>>& headers) {
  std::vector<std::string> lines;
  lines.reserve(headers.size());
  for (const auto& [k, v] : headers) {
    lines.push_back(k + ": " + v);
  }
  return lines;
}

std::optional<std::string> stringformat_simple(std::string_view value,
                                               std::string_view spec) {
  // Reject dangerous/complex specs.
  if (spec.empty() || spec.size() > 16) {
    return std::nullopt;
  }
  for (char c : spec) {
    if (c == '*' || c == '%' || c == '(') {
      return std::nullopt;
    }
  }
  char type = spec.back();
  // Only simple types
  const char* ok = "sdiufFeEgGxXoc";
  bool type_ok = false;
  for (const char* p = ok; *p; ++p) {
    if (*p == type) {
      type_ok = true;
      break;
    }
  }
  if (!type_ok) {
    return std::nullopt;
  }
  if (type == 's') {
    // %s with optional width/precision — only plain s
    if (spec == "s") {
      return std::string(value);
    }
    return std::nullopt;
  }
  // numeric
  char* end = nullptr;
  std::string tmp(value);
  if (type == 'd' || type == 'i' || type == 'u' || type == 'x' || type == 'X' ||
      type == 'o') {
    long long v = std::strtoll(tmp.c_str(), &end, 10);
    if (end == tmp.c_str() || *end != '\0') {
      return std::nullopt;
    }
    char fmt[32];
    std::snprintf(fmt, sizeof(fmt), "%%%s", std::string(spec).c_str());
    char buf[128];
    if (type == 'u' || type == 'x' || type == 'X' || type == 'o') {
      std::snprintf(buf, sizeof(buf), fmt, static_cast<unsigned long long>(v));
    } else {
      std::snprintf(buf, sizeof(buf), fmt, v);
    }
    return std::string(buf);
  }
  if (type == 'f' || type == 'F' || type == 'g' || type == 'G' || type == 'e' ||
      type == 'E') {
    double v = std::strtod(tmp.c_str(), &end);
    if (end == tmp.c_str() || *end != '\0' || !std::isfinite(v)) {
      return std::nullopt;
    }
    char fmt[32];
    std::snprintf(fmt, sizeof(fmt), "%%%s", std::string(spec).c_str());
    char buf[256];
    std::snprintf(buf, sizeof(buf), fmt, v);
    return std::string(buf);
  }
  return std::nullopt;
}

std::optional<std::string> floatformat_ascii(std::string_view decimal_str, int p) {
  if (decimal_str.empty() || decimal_str.size() > 200) {
    return std::nullopt;
  }
  for (char c : decimal_str) {
    if (!((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+')) {
      return std::nullopt;
    }
  }
  char* end = nullptr;
  std::string tmp(decimal_str);
  double v = std::strtod(tmp.c_str(), &end);
  if (end == tmp.c_str() || *end != '\0' || !std::isfinite(v)) {
    return std::nullopt;
  }
  double intpart = 0;
  double frac = std::modf(v, &intpart);
  const bool is_whole = std::fabs(frac) < 1e-12 * std::max(1.0, std::fabs(v));

  if (p <= 0 && is_whole) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.0f", intpart);
    return std::string(buf);
  }
  if (p < 0) {
    // Show up to |p| decimals but drop trailing zeros if whole after round —
    // full Django uses Decimal quantize; approximate with snprintf then trim.
    int prec = std::abs(p);
    if (prec > 20) {
      return std::nullopt;
    }
    char fmt[16];
    std::snprintf(fmt, sizeof(fmt), "%%.%df", prec);
    char buf[128];
    std::snprintf(buf, sizeof(buf), fmt, v);
    std::string s(buf);
    // trim trailing zeros after decimal
    auto dot = s.find('.');
    if (dot != std::string::npos) {
      while (!s.empty() && s.back() == '0') {
        s.pop_back();
      }
      if (!s.empty() && s.back() == '.') {
        s.pop_back();
      }
    }
    return s;
  }
  int prec = p;
  if (prec > 20) {
    return std::nullopt;
  }
  char fmt[16];
  std::snprintf(fmt, sizeof(fmt), "%%.%df", prec);
  char buf[128];
  std::snprintf(buf, sizeof(buf), fmt, v);
  return std::string(buf);
}

std::string sql_join_dotted(const std::vector<std::string>& parts) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) {
      out += '.';
    }
    out += parts[i];
  }
  return out;
}

std::string sql_pattern_wrap(std::string_view value, std::string_view kind) {
  std::string out;
  if (kind == "contains" || kind == "icontains") {
    out.reserve(value.size() + 2);
    out += '%';
    out.append(value);
    out += '%';
    return out;
  }
  if (kind == "startswith" || kind == "istartswith") {
    out.reserve(value.size() + 1);
    out.append(value);
    out += '%';
    return out;
  }
  if (kind == "endswith" || kind == "iendswith") {
    out.reserve(value.size() + 1);
    out += '%';
    out.append(value);
    return out;
  }
  return std::string(value);
}

bool choice_valid_value(std::string_view text_value,
                        const std::vector<std::string>& choice_keys) {
  for (const auto& k : choice_keys) {
    if (k == text_value) {
      return true;
    }
  }
  return false;
}

bool is_decimal_string(std::string_view value) noexcept {
  if (value.empty()) {
    return false;
  }
  std::size_t i = 0;
  if (value[i] == '+' || value[i] == '-') {
    ++i;
  }
  if (i >= value.size()) {
    return false;
  }
  bool saw_digit = false;
  bool saw_dot = false;
  for (; i < value.size(); ++i) {
    char c = value[i];
    if (c >= '0' && c <= '9') {
      saw_digit = true;
    } else if (c == '.' && !saw_dot) {
      saw_dot = true;
    } else if (c == 'e' || c == 'E') {
      // scientific — allow simple form
      ++i;
      if (i < value.size() && (value[i] == '+' || value[i] == '-')) {
        ++i;
      }
      if (i >= value.size()) {
        return false;
      }
      bool exp_digit = false;
      for (; i < value.size(); ++i) {
        if (value[i] < '0' || value[i] > '9') {
          return false;
        }
        exp_digit = true;
      }
      return saw_digit && exp_digit;
    } else {
      return false;
    }
  }
  return saw_digit;
}

std::optional<double> form_float_to_python(std::string_view value) {
  if (value.empty() || !is_decimal_string(value)) {
    // still try strtod for values is_decimal_string rejects wrongly
  }
  char* end = nullptr;
  std::string tmp(value);
  double v = std::strtod(tmp.c_str(), &end);
  if (end == tmp.c_str() || *end != '\0' || !std::isfinite(v)) {
    return std::nullopt;
  }
  return v;
}

bool is_valid_session_key(std::string_view key, int min_length,
                          bool check_charset) noexcept {
  if (key.empty() || static_cast<int>(key.size()) < min_length) {
    return false;
  }
  if (check_charset) {
    for (unsigned char c : key) {
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
        return false;
      }
    }
  }
  return true;
}

bool is_valid_samesite(std::string_view value) noexcept {
  if (value.empty()) {
    return true;
  }
  std::string low = ascii_lower(value);
  return low == "lax" || low == "none" || low == "strict";
}

bool cookie_delete_secure(std::string_view key, std::string_view samesite) noexcept {
  if (key.size() >= 9) {
    // __Secure- or __Host-
    if (key.rfind("__Secure-", 0) == 0 || key.rfind("__Host-", 0) == 0) {
      return true;
    }
  }
  if (!samesite.empty() && ascii_lower(samesite) == "none") {
    return true;
  }
  return false;
}

int cookie_max_age_seconds(double total_seconds) noexcept {
  // Match Python int(total_seconds) truncation toward zero.
  if (total_seconds > 2147483647.0) {
    return 2147483647;
  }
  if (total_seconds < -2147483648.0) {
    return -2147483648;
  }
  return static_cast<int>(total_seconds);
}

std::vector<std::string> signing_split(std::string_view signed_value,
                                       std::string_view sep) {
  std::vector<std::string> parts;
  if (sep.empty()) {
    return parts;
  }
  std::size_t start = 0;
  while (start <= signed_value.size()) {
    auto pos = signed_value.find(sep, start);
    if (pos == std::string_view::npos) {
      parts.emplace_back(signed_value.substr(start));
      break;
    }
    parts.emplace_back(signed_value.substr(start, pos - start));
    start = pos + sep.size();
    if (start == signed_value.size()) {
      parts.emplace_back("");
      break;
    }
  }
  return parts;
}

bool signing_is_compressed(std::string_view b64_payload) noexcept {
  return !b64_payload.empty() && b64_payload.front() == '.';
}

int where_child_outcome(int child_kind, bool negated, int& full_needed,
                        int& empty_needed) {
  // child_kind: 0=ok_sql, 1=EmptyResultSet, 2=FullResultSet, 3=empty sql
  if (child_kind == 1) {
    empty_needed -= 1;
  } else if (child_kind == 2) {
    full_needed -= 1;
  } else if (child_kind == 3) {
    full_needed -= 1;
  }
  // else ok_sql: no counter change beyond caller adding to result
  if (empty_needed == 0) {
    return negated ? 2 : 1;  // Full vs Empty
  }
  if (full_needed == 0) {
    return negated ? 1 : 2;
  }
  return 0;
}

std::string sql_comma_join(const std::vector<std::string>& parts) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) {
      out += ", ";
    }
    out += parts[i];
  }
  return out;
}

std::string sql_order_by_clause(const std::vector<std::string>& parts) {
  if (parts.empty()) {
    return {};
  }
  return "ORDER BY " + sql_comma_join(parts);
}

std::string sql_group_by_clause(const std::vector<std::string>& parts) {
  if (parts.empty()) {
    return {};
  }
  return "GROUP BY " + sql_comma_join(parts);
}

std::string sql_expr_as(std::string_view expr_sql, std::string_view quoted_alias) {
  if (quoted_alias.empty()) {
    return std::string(expr_sql);
  }
  std::string out;
  out.reserve(expr_sql.size() + quoted_alias.size() + 4);
  out.append(expr_sql);
  out += " AS ";
  out.append(quoted_alias);
  return out;
}

std::string sql_limit_offset_clause(std::optional<int> limit, int offset) {
  std::string out;
  if (limit.has_value() && *limit != 0) {
    // limit can be no_limit_value (-1 on SQLite); still emit LIMIT
    out = "LIMIT " + std::to_string(*limit);
  }
  if (offset) {
    if (!out.empty()) {
      out += ' ';
    }
    out += "OFFSET " + std::to_string(offset);
  }
  return out;
}

std::string join_promoter_effective_connector(std::string_view connector,
                                              bool negated) {
  if (!negated) {
    return std::string(connector);
  }
  if (connector == "AND") {
    return "OR";
  }
  // OR or XOR under NOT → treat like AND for join promotion (Django uses AND)
  return "AND";
}

bool join_promoter_should_promote(std::string_view effective_connector, int votes,
                                  int num_children) noexcept {
  // OR and not contained in all children → promote to LOUTER
  return effective_connector == "OR" && votes < num_children;
}

bool join_promoter_should_demote(std::string_view effective_connector, int votes,
                                 int num_children) noexcept {
  // AND → demote (inner); OR only if all children voted
  if (effective_connector == "AND") {
    return true;  // any vote demotes when AND (caller only iterates voted tables)
  }
  return effective_connector == "OR" && votes == num_children;
}

bool quote_name_is_alias(bool in_alias_map_not_table, bool in_extra_select,
                         bool external_alias_not_table) noexcept {
  return in_alias_map_not_table || in_extra_select || external_alias_not_table;
}

bool q_is_empty(int n_children) noexcept {
  return n_children <= 0;
}

std::vector<std::string> split_lookup_path(std::string_view path) {
  std::vector<std::string> parts;
  if (path.empty()) {
    return parts;
  }
  std::size_t start = 0;
  while (start <= path.size()) {
    auto pos = path.find("__", start);
    if (pos == std::string_view::npos) {
      parts.emplace_back(path.substr(start));
      break;
    }
    parts.emplace_back(path.substr(start, pos - start));
    start = pos + 2;
  }
  return parts;
}

std::string lookup_path_head(std::string_view path) {
  auto pos = path.find("__");
  if (pos == std::string_view::npos) {
    return std::string(path);
  }
  return std::string(path.substr(0, pos));
}

int q_combine_empty_flags(bool self_empty, bool other_empty) noexcept {
  if (self_empty) {
    return 1;
  }
  if (other_empty) {
    return 2;
  }
  return 0;
}

std::string join_lookup_path(const std::vector<std::string>& parts) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) {
      out += "__";
    }
    out += parts[i];
  }
  return out;
}

std::vector<std::string> lookup_field_parts(
    const std::vector<std::string>& lookup_splitted, int n_lookup_parts) {
  const int n = static_cast<int>(lookup_splitted.size());
  int keep = n - n_lookup_parts;
  if (keep < 0) {
    keep = 0;
  }
  if (keep > n) {
    keep = n;
  }
  return std::vector<std::string>(lookup_splitted.begin(),
                                  lookup_splitted.begin() + keep);
}

std::vector<std::string> lookup_or_exact(const std::vector<std::string>& lookups) {
  if (lookups.empty()) {
    return {"exact"};
  }
  return lookups;
}

RefsExpressionResult refs_expression_match(
    const std::vector<std::string>& lookup_parts,
    const std::vector<std::string>& annotation_keys) {
  RefsExpressionResult r;
  // Use a set-like scan; annotations can be numerous but typically small.
  for (std::size_t n = 1; n <= lookup_parts.size(); ++n) {
    std::string key = join_lookup_path(
        std::vector<std::string>(lookup_parts.begin(), lookup_parts.begin() + n));
    for (const auto& ann : annotation_keys) {
      if (ann == key) {
        r.annotation = std::move(key);
        r.remaining.assign(lookup_parts.begin() + static_cast<std::ptrdiff_t>(n),
                           lookup_parts.end());
        return r;
      }
    }
  }
  return r;
}

std::string next_numbered_alias(std::string_view prefix, int alias_map_size) {
  return std::string(prefix) + std::to_string(alias_map_size + 1);
}

int alias_refcount_add(int current, int amount, bool clamp_non_negative) noexcept {
  long long v = static_cast<long long>(current) + static_cast<long long>(amount);
  if (clamp_non_negative && v < 0) {
    return 0;
  }
  if (v > 2147483647LL) {
    return 2147483647;
  }
  if (v < -2147483648LL) {
    return -2147483648;
  }
  return static_cast<int>(v);
}

std::vector<std::string> alias_refcount_increased(
    const std::vector<std::pair<std::string, int>>& pre,
    const std::vector<std::pair<std::string, int>>& post) {
  std::vector<std::string> out;
  for (const auto& [alias, post_c] : post) {
    int pre_c = 0;
    for (const auto& [a, c] : pre) {
      if (a == alias) {
        pre_c = c;
        break;
      }
    }
    if (post_c > pre_c) {
      out.push_back(alias);
    }
  }
  return out;
}

bool lookup_invalid_without_field(int n_lookup_parts, int n_field_parts) noexcept {
  return n_lookup_parts > 1 && n_field_parts == 0;
}

OrderBySplit split_order_by_item(std::string_view item) {
  OrderBySplit r;
  if (!item.empty() && item.front() == '-') {
    r.descending = true;
    r.field = std::string(item.substr(1));
  } else {
    r.field = std::string(item);
  }
  return r;
}

int values_list_flags(bool flat, bool named, int n_fields) noexcept {
  if (flat && named) {
    return 1;
  }
  if (flat && n_fields > 1) {
    return 2;
  }
  return 0;
}

std::string unique_field_alias(std::string_view base, int start_counter,
                               const std::vector<std::string>& existing_keys) {
  // Match Django values_list:
  //   while (field_name := f"{prefix}{counter}") in field_names: counter += 1
  // Always forms prefix+counter first (even if bare prefix is free).
  auto exists = [&](const std::string& k) {
    for (const auto& e : existing_keys) {
      if (e == k) {
        return true;
      }
    }
    return false;
  };
  int counter = start_counter;
  std::string candidate = std::string(base) + std::to_string(counter);
  while (exists(candidate)) {
    ++counter;
    candidate = std::string(base) + std::to_string(counter);
  }
  return candidate;
}

bool result_cache_truthy(bool has_cache, bool cache_nonempty) noexcept {
  return has_cache && cache_nonempty;
}

std::string session_cache_key(std::string_view prefix, std::string_view session_key) {
  std::string out;
  out.reserve(prefix.size() + session_key.size());
  out.append(prefix);
  out.append(session_key);
  return out;
}

int session_expiry_age_seconds(int cookie_age, std::optional<int> modification_age,
                               std::optional<int> expiry) noexcept {
  // Django get_expiry_age:
  //   if not expiry: return cookie_age   (None or 0)
  //   if int-like: return expiry as-is   (remaining seconds)
  // This helper implements the not-expiry branch when expiry is nullopt/0,
  // and returns *expiry when set and non-zero (int remaining path).
  // Callers with datetime use session_delta_seconds instead.
  if (!expiry.has_value() || *expiry == 0) {
    return cookie_age;
  }
  (void)modification_age;
  return *expiry;
}

std::int64_t session_delta_seconds(std::int64_t days, std::int64_t seconds) noexcept {
  // Match datetime.timedelta.days * 86400 + .seconds (seconds always 0..86399).
  return days * 86400 + seconds;
}

bool session_key_missing(std::string_view session_key) noexcept {
  return session_key.empty();
}

std::string sql_for_update(bool no_key, bool nowait, bool skip_locked,
                           const std::vector<std::string>& of) {
  // FOR[ NO KEY] UPDATE[ OF ...][ NOWAIT][ SKIP LOCKED]
  std::string out = "FOR";
  if (no_key) {
    out += " NO KEY";
  }
  out += " UPDATE";
  if (!of.empty()) {
    out += " OF ";
    out += sql_comma_join(of);
  }
  if (nowait) {
    out += " NOWAIT";
  }
  if (skip_locked) {
    out += " SKIP LOCKED";
  }
  return out;
}

std::string sql_combinator_keyword(std::string_view combinator, bool all) {
  std::string c;
  if (combinator == "union") {
    c = "UNION";
  } else if (combinator == "intersection") {
    c = "INTERSECT";
  } else if (combinator == "difference") {
    c = "EXCEPT";
  } else {
    // Pass through uppercased
    c.reserve(combinator.size());
    for (char ch : combinator) {
      if (ch >= 'a' && ch <= 'z') {
        c += static_cast<char>(ch - 'a' + 'A');
      } else {
        c += ch;
      }
    }
  }
  if (all && combinator == "union") {
    c += " ALL";
  }
  return c;
}

std::string sql_combinator_join(std::string_view combinator_sql,
                                const std::vector<std::string>& parts,
                                bool wrap_parens) {
  if (parts.empty()) {
    return {};
  }
  std::string sep = " ";
  sep.append(combinator_sql);
  sep += ' ';
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) {
      out += sep;
    }
    if (wrap_parens) {
      out += '(';
      out += parts[i];
      out += ')';
    } else {
      out += parts[i];
    }
  }
  return out;
}

std::string sql_distinct_clause(const std::vector<std::string>& fields, bool allow_on) {
  if (fields.empty()) {
    return "DISTINCT";
  }
  if (!allow_on) {
    return {};  // signal unsupported
  }
  return "DISTINCT ON (" + sql_comma_join(fields) + ")";
}

bool multi_choice_has_changed(const std::vector<std::string>& initial,
                              const std::vector<std::string>& data) noexcept {
  if (initial.size() != data.size()) {
    return true;
  }
  // set equality
  for (const auto& d : data) {
    bool found = false;
    for (const auto& i : initial) {
      if (i == d) {
        found = true;
        break;
      }
    }
    if (!found) {
      return true;
    }
  }
  for (const auto& i : initial) {
    bool found = false;
    for (const auto& d : data) {
      if (i == d) {
        found = true;
        break;
      }
    }
    if (!found) {
      return true;
    }
  }
  return false;
}

bool json_looks_valid(std::string_view value) noexcept {
  // Strip leading whitespace
  std::size_t i = 0;
  while (i < value.size() &&
         (value[i] == ' ' || value[i] == '\t' || value[i] == '\n' || value[i] == '\r')) {
    ++i;
  }
  if (i >= value.size()) {
    return false;
  }
  char c = value[i];
  if (c == '{' || c == '[' || c == '"' || c == '-' || (c >= '0' && c <= '9')) {
    return true;
  }
  // true / false / null
  auto rest = value.substr(i);
  if (rest.size() >= 4 && rest.substr(0, 4) == "true") {
    return true;
  }
  if (rest.size() >= 5 && rest.substr(0, 5) == "false") {
    return true;
  }
  if (rest.size() >= 4 && rest.substr(0, 4) == "null") {
    return true;
  }
  return false;
}

std::vector<std::string> stringify_choice_list(
    const std::vector<std::string>& values) {
  return values;  // already strings from Python
}

std::string sql_from_tables(const std::vector<std::string>& clauses) {
  if (clauses.empty()) {
    return {};
  }
  std::string out = "FROM ";
  for (std::size_t i = 0; i < clauses.size(); ++i) {
    if (i) {
      out += ' ';
    }
    out += clauses[i];
  }
  return out;
}

int queryset_count_from_cache(bool has_cache, int cache_len) noexcept {
  if (!has_cache) {
    return -1;
  }
  return cache_len < 0 ? 0 : cache_len;
}

bool queryset_exists_from_cache(bool has_cache, bool cache_nonempty) noexcept {
  return has_cache && cache_nonempty;
}

bool queryset_use_cache_for_first_last(bool has_cache, bool ordered,
                                       bool cache_nonempty) noexcept {
  // Only safe when ordered: unordered first() reorders by pk.
  (void)cache_nonempty;
  return has_cache && ordered;
}

int iterator_chunk_validate(bool chunk_size_none, int chunk_size,
                            bool has_prefetch) noexcept {
  if (chunk_size_none) {
    if (has_prefetch) {
      return 1;  // must provide chunk_size
    }
    return 0;
  }
  if (chunk_size <= 0) {
    return 2;
  }
  return 0;
}

int iterator_chunk_size_or_default(bool chunk_size_none, int chunk_size,
                                   int default_size) noexcept {
  if (chunk_size_none) {
    return default_size > 0 ? default_size : 2000;
  }
  return chunk_size;
}

bool in_bulk_empty(bool id_list_is_none, int id_list_len) noexcept {
  return !id_list_is_none && id_list_len == 0;
}

std::string in_bulk_filter_key(std::string_view field_name) {
  std::string out;
  out.reserve(field_name.size() + 4);
  out.append(field_name);
  out += "__in";
  return out;
}

std::vector<BatchRange> in_bulk_batch_ranges(int n_ids, int batch_size) {
  std::vector<BatchRange> out;
  if (n_ids <= 0) {
    return out;
  }
  if (batch_size <= 0 || batch_size >= n_ids) {
    out.push_back(BatchRange{0, n_ids});
    return out;
  }
  for (int offset = 0; offset < n_ids; offset += batch_size) {
    int end = offset + batch_size;
    if (end > n_ids) {
      end = n_ids;
    }
    out.push_back(BatchRange{offset, end});
  }
  return out;
}

int get_result_kind(int num_results, int limit) noexcept {
  if (num_results == 1) {
    return 0;
  }
  if (num_results == 0) {
    return 1;
  }
  return 2;  // multiple (limit used only for message text in Python)
  (void)limit;
}

std::string bulk_insert_sql(const std::vector<std::string>& row_sqls) {
  if (row_sqls.empty()) {
    return "VALUES ";
  }
  std::string out = "VALUES ";
  for (std::size_t i = 0; i < row_sqls.size(); ++i) {
    if (i) {
      out += ", ";
    }
    out += '(';
    out += row_sqls[i];
    out += ')';
  }
  return out;
}

std::string bulk_placeholder_row(const std::vector<std::string>& cols) {
  return sql_comma_join(cols);
}

int validate_positive_batch_size(bool is_none, int batch_size) noexcept {
  if (is_none) {
    return 0;
  }
  return batch_size > 0 ? 0 : 1;
}

int effective_batch_size(bool user_set, int user_batch, int max_batch,
                         int n_objs) noexcept {
  int maxb = max_batch > 0 ? max_batch : (n_objs > 0 ? n_objs : 1);
  if (!user_set) {
    return maxb;
  }
  if (user_batch <= 0) {
    return maxb;
  }
  return user_batch < maxb ? user_batch : maxb;
}

int queryset_write_guard(bool combinator, bool is_sliced, bool has_distinct_fields,
                         bool has_values_fields) noexcept {
  if (combinator) {
    return 1;
  }
  if (is_sliced) {
    return 2;
  }
  if (has_distinct_fields) {
    return 3;
  }
  if (has_values_fields) {
    return 4;
  }
  return 0;
}

std::string sql_update_set_clause(const std::vector<std::string>& assignments) {
  return sql_comma_join(assignments);
}

bool multi_batch_needs_atomic(int n_batches) noexcept {
  return n_batches > 1;
}

bool key_has_lookup_sep(std::string_view key) noexcept {
  return key.find("__") != std::string_view::npos;
}

std::vector<std::string> keys_without_lookup_sep(
    const std::vector<std::string>& keys) {
  std::vector<std::string> out;
  out.reserve(keys.size());
  for (const auto& k : keys) {
    if (!key_has_lookup_sep(k)) {
      out.push_back(k);
    }
  }
  return out;
}

bool create_defaults_use_update(bool create_defaults_is_none) noexcept {
  return create_defaults_is_none;
}

std::string join_sorted_comma(const std::vector<std::string>& names) {
  std::vector<std::string> sorted = names;
  std::sort(sorted.begin(), sorted.end());
  return sql_comma_join(sorted);
}

int bulk_create_conflict_kind(bool ignore_conflicts, bool update_conflicts) noexcept {
  if (ignore_conflicts && update_conflicts) {
    return -1;
  }
  if (ignore_conflicts) {
    return 1;
  }
  if (update_conflicts) {
    return 2;
  }
  return 0;
}

int contains_preflight(bool has_values_fields, bool pk_set) noexcept {
  if (has_values_fields) {
    return 1;
  }
  if (!pk_set) {
    return 2;
  }
  return 0;
}

bool aggregate_distinct_fields_error(bool has_distinct_fields) noexcept {
  return has_distinct_fields;
}

bool filter_after_slice_error(bool has_filters, bool is_sliced) noexcept {
  return has_filters && is_sliced;
}

std::vector<std::string> prohibited_filter_kwargs(
    const std::vector<std::string>& keys) {
  std::vector<std::string> out;
  for (const auto& k : keys) {
    if (k == "_connector" || k == "_negated") {
      out.push_back(k);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool select_for_update_options_conflict(bool nowait, bool skip_locked) noexcept {
  return nowait && skip_locked;
}

int union_empty_self_kind(int nonempty_other_count) noexcept {
  if (nonempty_other_count <= 0) {
    return 0;
  }
  if (nonempty_other_count == 1) {
    return 1;
  }
  return 2;
}

bool combinator_return_empty_self(bool self_is_empty) noexcept {
  return self_is_empty;
}

bool save_force_conflict(bool force_insert, bool force_update,
                         bool has_update_fields) noexcept {
  return force_insert && (force_update || has_update_fields);
}

bool save_skip_empty_update_fields(bool update_fields_is_none,
                                   int n_update_fields) noexcept {
  return !update_fields_is_none && n_update_fields <= 0;
}

bool save_force_update_no_pk(bool pk_set, bool force_update,
                             bool has_update_fields) noexcept {
  return !pk_set && (force_update || has_update_fields);
}

bool collector_add_empty(int n_objs) noexcept {
  return n_objs <= 0;
}

bool collector_delete_empty(int n_models, int n_fast_deletes) noexcept {
  return n_models <= 0 && n_fast_deletes <= 0;
}

bool collector_single_fast_path(int n_models, int n_instances) noexcept {
  return n_models == 1 && n_instances == 1;
}

bool can_fast_delete_result(bool from_field_blocks, bool model_ok,
                            bool has_signal_listeners, bool parents_ok,
                            bool relations_ok, bool no_bulk_related) noexcept {
  if (from_field_blocks || !model_ok || has_signal_listeners) {
    return false;
  }
  return parents_ok && relations_ok && no_bulk_related;
}

std::string sql_assignment(std::string_view quoted_col, std::string_view rhs) {
  std::string out;
  out.reserve(quoted_col.size() + rhs.size() + 3);
  out.append(quoted_col);
  out += " = ";
  out.append(rhs);
  return out;
}

std::string sql_null_assignment(std::string_view quoted_col) {
  std::string out;
  out.reserve(quoted_col.size() + 7);
  out.append(quoted_col);
  out += " = NULL";
  return out;
}

std::string sql_parenthesized_list(const std::vector<std::string>& cols) {
  return "(" + sql_comma_join(cols) + ")";
}

std::string sql_values_row(std::string_view placeholders) {
  std::string out = "VALUES (";
  out.append(placeholders);
  out += ')';
  return out;
}

std::string sql_aggregate_subquery(std::string_view select_sql,
                                   std::string_view inner_sql) {
  std::string out = "SELECT ";
  out.append(select_sql);
  out += " FROM (";
  out.append(inner_sql);
  out += ") subquery";
  return out;
}

std::string sql_space_join(const std::vector<std::string>& parts) {
  if (parts.empty()) {
    return {};
  }
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) {
      out += ' ';
    }
    out += parts[i];
  }
  return out;
}

int row_count_or_zero(bool is_none, int row_count) noexcept {
  if (is_none) {
    return 0;
  }
  return row_count;
}

bool queryset_sliced_error(bool is_sliced) noexcept {
  return is_sliced;
}

bool clear_none_arg(bool single_none) noexcept {
  return single_none;
}

bool only_none_arg_error(bool single_none) noexcept {
  return single_none;
}

bool reverse_standard_ordering(bool standard_ordering) noexcept {
  return !standard_ordering;
}

int queryset_index_validate(bool is_int, bool is_slice, bool has_negative) noexcept {
  if (!is_int && !is_slice) {
    return 1;
  }
  if (has_negative) {
    return 2;
  }
  return 0;
}

int qs_and_empty_kind(bool self_empty, bool other_empty) noexcept {
  if (other_empty) {
    return 1;
  }
  if (self_empty) {
    return 2;
  }
  return 0;
}

int qs_or_empty_kind(bool self_empty, bool other_empty) noexcept {
  if (self_empty) {
    return 1;
  }
  if (other_empty) {
    return 2;
  }
  return 0;
}

bool date_kind_valid(std::string_view kind) noexcept {
  return kind == "year" || kind == "month" || kind == "week" || kind == "day";
}

bool datetime_kind_valid(std::string_view kind) noexcept {
  return kind == "year" || kind == "month" || kind == "week" || kind == "day" ||
         kind == "hour" || kind == "minute" || kind == "second";
}

bool date_order_valid(std::string_view order) noexcept {
  return order == "ASC" || order == "DESC";
}

std::string order_by_desc_prefix(std::string_view order) {
  return order == "DESC" ? std::string("-") : std::string();
}

bool earliest_missing_fields(bool has_fields, bool has_get_latest_by) noexcept {
  return !has_fields && !has_get_latest_by;
}

bool save_base_needs_atomic(bool has_parents) noexcept {
  return has_parents;
}

bool save_created_flag(bool updated) noexcept {
  return !updated;
}

int do_update_empty_values_kind(bool has_update_fields, bool exists) noexcept {
  return (has_update_fields || exists) ? 0 : 1;
}

bool clean_field_skip(bool name_in_exclude, bool generated) noexcept {
  return name_in_exclude || generated;
}

bool clean_field_skip_blank_empty(bool blank, bool in_empty_values) noexcept {
  return blank && in_empty_values;
}

bool validation_has_errors(int n_error_keys) noexcept {
  return n_error_keys > 0;
}

bool is_non_field_errors_key(std::string_view name) noexcept {
  return name == "__all__";
}

std::string fixed_timezone_name(int offset_minutes) {
  const int abs_m = offset_minutes < 0 ? -offset_minutes : offset_minutes;
  const int hh = abs_m / 60;
  const int mm = abs_m % 60;
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%c%02d%02d", offset_minutes < 0 ? '-' : '+', hh, mm);
  return std::string(buf);
}

bool datetime_is_aware(bool utcoffset_not_none) noexcept {
  return utcoffset_not_none;
}

bool datetime_is_naive(bool utcoffset_is_none) noexcept {
  return utcoffset_is_none;
}

int mark_safe_kind(bool has_html, bool is_callable) noexcept {
  if (has_html) {
    return 0;
  }
  if (is_callable) {
    return 1;
  }
  return 2;
}

std::string lookup_head(std::string_view lookup) {
  const auto pos = lookup.find("__");
  if (pos == std::string_view::npos) {
    return std::string(lookup);
  }
  return std::string(lookup.substr(0, pos));
}

bool queryset_is_ordered(bool is_empty_qs, bool has_extra_order, bool has_order_by,
                         bool default_ordering, bool has_meta_ordering,
                         bool has_group_by) noexcept {
  if (is_empty_qs) {
    return true;
  }
  if (has_extra_order || has_order_by) {
    return true;
  }
  if (default_ordering && has_meta_ordering && !has_group_by) {
    return true;
  }
  return false;
}

bool annotation_alias_conflicts(bool alias_in_names) noexcept {
  return alias_in_names;
}

bool complex_filter_is_q(bool is_q_instance) noexcept {
  return is_q_instance;
}

bool using_is_none(bool using_is_none) noexcept {
  return using_is_none;
}

bool refresh_fields_empty(int n_fields) noexcept {
  return n_fields <= 0;
}

bool refresh_fields_have_lookup_sep(const std::vector<std::string>& fields) {
  for (const auto& f : fields) {
    if (key_has_lookup_sep(f)) {
      return true;
    }
  }
  return false;
}

bool unique_check_excluded(const std::vector<std::string>& check_names,
                           const std::vector<std::string>& exclude) {
  for (const auto& name : check_names) {
    for (const auto& ex : exclude) {
      if (name == ex) {
        return true;
      }
    }
  }
  return false;
}

bool unique_lookup_skip_value(bool is_none, bool is_empty_str,
                              bool empty_as_null) noexcept {
  return is_none || (is_empty_str && empty_as_null);
}

bool unique_check_incomplete(int n_check, int n_kwargs) noexcept {
  return n_check != n_kwargs;
}

bool unique_error_is_single_field(int n_check) noexcept {
  return n_check == 1;
}

bool in_lookup_empty(int n_rhs) noexcept {
  return n_rhs <= 0;
}

std::string sql_lhs_rhs(std::string_view lhs, std::string_view rhs_op) {
  std::string out;
  out.reserve(lhs.size() + rhs_op.size() + 1);
  out.append(lhs);
  out += ' ';
  out.append(rhs_op);
  return out;
}

std::string sql_or_join(const std::vector<std::string>& parts) {
  if (parts.empty()) {
    return {};
  }
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) {
      out += " OR ";
    }
    out += parts[i];
  }
  return out;
}

bool is_password_usable(bool encoded_is_none, bool starts_with_unusable) noexcept {
  return encoded_is_none || !starts_with_unusable;
}

int identify_hasher_kind(int encoded_len, bool has_dollar, bool starts_md5_dollar,
                         bool starts_sha1_dollar) noexcept {
  if ((encoded_len == 32 && !has_dollar) || (encoded_len == 37 && starts_md5_dollar)) {
    return 1;
  }
  if (encoded_len == 46 && starts_sha1_dollar) {
    return 2;
  }
  return 0;
}

std::string hasher_algorithm_prefix(std::string_view encoded) {
  const auto pos = encoded.find('$');
  if (pos == std::string_view::npos) {
    return std::string(encoded);
  }
  return std::string(encoded.substr(0, pos));
}

std::string cache_default_key(std::string_view key_prefix, int version,
                              std::string_view key) {
  std::string out;
  out.reserve(key_prefix.size() + key.size() + 16);
  out.append(key_prefix);
  out += ':';
  out += std::to_string(version);
  out += ':';
  out.append(key);
  return out;
}

int cache_timeout_kind(bool is_default_sentinel, bool is_none, int timeout) noexcept {
  if (is_default_sentinel) {
    return 0;
  }
  if (is_none) {
    return 2;
  }
  if (timeout == 0) {
    return 1;
  }
  return 3;
}

bool file_multiple_chunks(std::int64_t size, std::int64_t chunk_size) noexcept {
  return size > chunk_size;
}

std::string mask_hash(std::string_view hash, int show, char mask_char) {
  if (show < 0) {
    show = 0;
  }
  if (static_cast<std::size_t>(show) >= hash.size()) {
    return std::string(hash);
  }
  std::string out;
  out.reserve(hash.size());
  out.append(hash.substr(0, static_cast<std::size_t>(show)));
  out.append(hash.size() - static_cast<std::size_t>(show), mask_char);
  return out;
}

namespace {

constexpr std::string_view kCsrfChars =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

[[nodiscard]] int csrf_char_index(char c) noexcept {
  if (c >= 'a' && c <= 'z') {
    return c - 'a';
  }
  if (c >= 'A' && c <= 'Z') {
    return 26 + (c - 'A');
  }
  if (c >= '0' && c <= '9') {
    return 52 + (c - '0');
  }
  return -1;
}

}  // namespace

bool result_cache_populated(bool cache_is_none) noexcept {
  return !cache_is_none;
}

bool prefetch_still_needed(bool has_lookups, bool prefetch_done) noexcept {
  return has_lookups && !prefetch_done;
}

bool queryset_cache_truthy(int cache_len) noexcept {
  return cache_len > 0;
}

bool sticky_filter_active(bool sticky) noexcept {
  return sticky;
}

int csrf_token_length_ok(int len, int secret_len, int token_len) noexcept {
  if (len == secret_len || len == token_len) {
    return 0;
  }
  return 1;
}

bool csrf_token_chars_valid(std::string_view token) noexcept {
  for (char c : token) {
    if (csrf_char_index(c) < 0) {
      return false;
    }
  }
  return true;
}

std::string csrf_unmask_token(std::string_view token, int secret_len) {
  if (secret_len <= 0 ||
      token.size() != static_cast<std::size_t>(2 * secret_len)) {
    return {};
  }
  std::string out;
  out.reserve(static_cast<std::size_t>(secret_len));
  for (int i = 0; i < secret_len; ++i) {
    const int x = csrf_char_index(token[static_cast<std::size_t>(secret_len + i)]);
    const int y = csrf_char_index(token[static_cast<std::size_t>(i)]);
    if (x < 0 || y < 0) {
      return {};
    }
    // Python: chars[x - y] with negative ok via Python list indexing.
    int idx = x - y;
    const int n = static_cast<int>(kCsrfChars.size());
    idx %= n;
    if (idx < 0) {
      idx += n;
    }
    out += kCsrfChars[static_cast<std::size_t>(idx)];
  }
  return out;
}

std::string csrf_mask_secret(std::string_view secret, std::string_view mask) {
  if (secret.size() != mask.size() || secret.empty()) {
    return {};
  }
  std::string cipher;
  cipher.reserve(secret.size());
  const int n = static_cast<int>(kCsrfChars.size());
  for (std::size_t i = 0; i < secret.size(); ++i) {
    const int x = csrf_char_index(secret[i]);
    const int y = csrf_char_index(mask[i]);
    if (x < 0 || y < 0) {
      return {};
    }
    cipher += kCsrfChars[static_cast<std::size_t>((x + y) % n)];
  }
  std::string out;
  out.reserve(mask.size() + cipher.size());
  out.append(mask);
  out += cipher;
  return out;
}

std::string hsts_header_value(int seconds, bool include_subdomains, bool preload) {
  std::string out = "max-age=";
  out += std::to_string(seconds);
  if (include_subdomains) {
    out += "; includeSubDomains";
  }
  if (preload) {
    out += "; preload";
  }
  return out;
}

std::string https_redirect_url(std::string_view host, std::string_view full_path) {
  std::string out = "https://";
  out.append(host);
  out.append(full_path);
  return out;
}

std::string referrer_policy_header(const std::vector<std::string>& policies) {
  // Referrer-Policy must use "," without spaces (Django stock join).
  if (policies.empty()) {
    return {};
  }
  std::string out;
  for (std::size_t i = 0; i < policies.size(); ++i) {
    if (i) {
      out += ',';
    }
    out += policies[i];
  }
  return out;
}

bool route_looks_like_regex(std::string_view route) noexcept {
  if (route.find("(?P<") != std::string_view::npos) {
    return true;
  }
  if (!route.empty() && route.front() == '^') {
    return true;
  }
  if (!route.empty() && route.back() == '$') {
    return true;
  }
  return false;
}

RouteSimpleMatch route_simple_match(bool is_endpoint, std::string_view route,
                                    std::string_view path) {
  RouteSimpleMatch m;
  if (is_endpoint) {
    if (route == path) {
      m.kind = 1;
      m.remaining = "";
    }
    return m;
  }
  if (path.size() >= route.size() && path.substr(0, route.size()) == route) {
    m.kind = 2;
    m.remaining = std::string(path.substr(route.size()));
  }
  return m;
}

bool engine_loaders_app_dirs_conflict(bool app_dirs, bool loaders_defined) noexcept {
  return app_dirs && loaders_defined;
}

std::string template_cache_key_plain(std::string_view template_name) {
  return std::string(template_name);
}

std::string to_language(std::string_view locale) {
  std::string out;
  out.reserve(locale.size());
  const auto p = locale.find('_');
  if (p == std::string_view::npos) {
    for (char c : locale) {
      out += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    return out;
  }
  for (std::size_t i = 0; i < p; ++i) {
    char c = locale[i];
    out += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  }
  out += '-';
  for (std::size_t i = p + 1; i < locale.size(); ++i) {
    char c = locale[i];
    out += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  }
  return out;
}

std::string to_locale(std::string_view language) {
  // lang, _, country = language.lower().partition("-")
  std::string lower;
  lower.reserve(language.size());
  for (char c : language) {
    lower += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  }
  const auto dash = lower.find('-');
  if (dash == std::string::npos) {
    // language[:3].lower() + language[3:]  (only first 3 lowercased in stock
    // when no country — actually whole language lower is already done above
    // for partition; stock: return language[:3].lower() + language[3:])
    std::string out;
    out.reserve(language.size());
    for (std::size_t i = 0; i < language.size(); ++i) {
      char c = language[i];
      if (i < 3) {
        out += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
      } else {
        out += c;
      }
    }
    return out;
  }
  std::string lang = lower.substr(0, dash);
  std::string rest = lower.substr(dash + 1);
  std::string country;
  std::string tail;
  const auto dash2 = rest.find('-');
  if (dash2 == std::string::npos) {
    country = rest;
  } else {
    country = rest.substr(0, dash2);
    tail = rest.substr(dash2 + 1);
  }
  // title if len > 2 else upper
  if (country.size() > 2) {
    if (!country.empty()) {
      country[0] = static_cast<char>(
          country[0] >= 'a' && country[0] <= 'z' ? country[0] - 'a' + 'A' : country[0]);
      for (std::size_t i = 1; i < country.size(); ++i) {
        char c = country[i];
        country[i] =
            static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
      }
    }
  } else {
    for (char& c : country) {
      c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    }
  }
  std::string out = lang + "_" + country;
  if (!tail.empty()) {
    out += "-";
    out += tail;
  }
  return out;
}

int plural_index_default(int n) noexcept {
  return n != 1 ? 1 : 0;
}

bool language_code_too_long(int len, int max_len) noexcept {
  return len > max_len;
}

std::string sql_create_table(std::string_view quoted_table,
                             std::string_view columns_sql) {
  std::string out = "CREATE TABLE ";
  out.append(quoted_table);
  out += " (";
  out.append(columns_sql);
  out += ")";
  return out;
}

std::string migration_describe(std::string_view class_name,
                               std::string_view constructor_args) {
  std::string out;
  out.reserve(class_name.size() + constructor_args.size() + 2);
  out.append(class_name);
  out += ": ";
  out.append(constructor_args);
  return out;
}

std::string migration_formatted_description(std::string_view category,
                                            std::string_view description) {
  std::string out;
  out.reserve(category.size() + description.size() + 1);
  out.append(category);
  out += ' ';
  out.append(description);
  return out;
}

bool http_status_code_valid(int code) noexcept {
  return code >= 100 && code <= 599;
}

std::string weak_etag_if_strong(std::string_view etag) {
  if (!etag.empty() && etag.front() == '"') {
    std::string out = "W/";
    out.append(etag);
    return out;
  }
  return std::string(etag);
}

bool accepts_gzip(std::string_view accept_encoding) noexcept {
  // Match \bgzip\b: gzip as a token (not part of a longer word).
  const std::size_t n = accept_encoding.size();
  for (std::size_t i = 0; i + 4 <= n; ++i) {
    if ((accept_encoding[i] == 'g' || accept_encoding[i] == 'G') &&
        (accept_encoding[i + 1] == 'z' || accept_encoding[i + 1] == 'Z') &&
        (accept_encoding[i + 2] == 'i' || accept_encoding[i + 2] == 'I') &&
        (accept_encoding[i + 3] == 'p' || accept_encoding[i + 3] == 'P')) {
      const bool left_ok = (i == 0) || !std::isalnum(static_cast<unsigned char>(
                                            accept_encoding[i - 1]));
      const bool right_ok =
          (i + 4 >= n) ||
          !std::isalnum(static_cast<unsigned char>(accept_encoding[i + 4]));
      if (left_ok && right_ok) {
        return true;
      }
    }
  }
  return false;
}

bool gzip_content_too_short(int content_len, int min_len) noexcept {
  return content_len < min_len;
}

bool host_needs_www_prefix(std::string_view host) noexcept {
  if (host.empty()) {
    return false;
  }
  if (host.size() >= 4 && host[0] == 'w' && host[1] == 'w' && host[2] == 'w' &&
      host[3] == '.') {
    return false;
  }
  return true;
}

std::string www_redirect_url(std::string_view scheme, std::string_view host,
                             std::string_view path) {
  std::string out;
  out.reserve(scheme.size() + host.size() + path.size() + 10);
  out.append(scheme);
  out += "://www.";
  out.append(host);
  out.append(path);
  return out;
}

std::string xframe_options_value(std::string_view setting_value) {
  if (setting_value.empty()) {
    return "DENY";
  }
  std::string out;
  out.reserve(setting_value.size());
  for (char c : setting_value) {
    if (c >= 'a' && c <= 'z') {
      out += static_cast<char>(c - 'a' + 'A');
    } else {
      out += c;
    }
  }
  return out;
}

std::string message_tags_join(std::string_view extra_tags,
                              std::string_view level_tag) {
  if (extra_tags.empty()) {
    return std::string(level_tag);
  }
  if (level_tag.empty()) {
    return std::string(extra_tags);
  }
  std::string out;
  out.reserve(extra_tags.size() + level_tag.size() + 1);
  out.append(extra_tags);
  out += ' ';
  out.append(level_tag);
  return out;
}

std::string hashed_static_basename(std::string_view root,
                                   std::string_view hash_with_dot,
                                   std::string_view ext) {
  std::string out;
  out.reserve(root.size() + hash_with_dot.size() + ext.size());
  out.append(root);
  out.append(hash_with_dot);
  out.append(ext);
  return out;
}

std::string posix_path_join(std::string_view directory, std::string_view basename) {
  if (directory.empty()) {
    return std::string(basename);
  }
  std::string out;
  out.reserve(directory.size() + basename.size() + 1);
  out.append(directory);
  if (directory.back() != '/') {
    out += '/';
  }
  out.append(basename);
  return out;
}

bool json_use_indent_separators(bool has_indent) noexcept {
  return has_indent;
}

std::string datetime_iso_utc_z(std::string_view iso) {
  constexpr std::string_view suffix = "+00:00";
  if (iso.size() >= suffix.size() &&
      iso.substr(iso.size() - suffix.size()) == suffix) {
    std::string out(iso.substr(0, iso.size() - suffix.size()));
    out += 'Z';
    return out;
  }
  return std::string(iso);
}

bool string_has_newlines(std::string_view s) noexcept {
  return s.find('\n') != std::string_view::npos ||
         s.find('\r') != std::string_view::npos;
}

EmailParts split_email_address(std::string_view address) {
  EmailParts p;
  const auto at = address.rfind('@');
  if (at == std::string_view::npos || at == 0 || at + 1 >= address.size()) {
    return p;
  }
  // Match str.rsplit("@", 1): local may itself contain '@'.
  p.local = std::string(address.substr(0, at));
  p.domain = std::string(address.substr(at + 1));
  p.ok = true;
  return p;
}

std::string model_meta_label(std::string_view app_label,
                             std::string_view object_name) {
  std::string out;
  out.reserve(app_label.size() + object_name.size() + 1);
  out.append(app_label);
  out += '.';
  out.append(object_name);
  return out;
}

std::string manager_str(std::string_view model_label, std::string_view manager_name) {
  std::string out;
  out.reserve(model_label.size() + manager_name.size() + 1);
  out.append(model_label);
  out += '.';
  out.append(manager_name);
  return out;
}

std::string from_queryset_class_name(std::string_view manager_cls,
                                     std::string_view qs_cls) {
  std::string out;
  out.reserve(manager_cls.size() + qs_cls.size() + 4);
  out.append(manager_cls);
  out += "From";
  out.append(qs_cls);
  return out;
}

std::string migration_node_key(std::string_view app_label, std::string_view name) {
  return model_meta_label(app_label, name);
}

std::string perm_codename(std::string_view action, std::string_view model_name) {
  std::string out;
  out.reserve(action.size() + model_name.size() + 1);
  out.append(action);
  out += '_';
  out.append(model_name);
  return out;
}

bool user_can_authenticate(bool has_is_active, bool is_active) noexcept {
  return !has_is_active || is_active;
}

bool signal_has_receivers(int n_receivers) noexcept {
  return n_receivers > 0;
}

DottedPathParts split_dotted_path(std::string_view dotted) {
  DottedPathParts p;
  const auto pos = dotted.rfind('.');
  if (pos == std::string_view::npos || pos == 0 || pos + 1 >= dotted.size()) {
    return p;
  }
  p.module = std::string(dotted.substr(0, pos));
  p.attr = std::string(dotted.substr(pos + 1));
  p.ok = true;
  return p;
}

std::string app_module_path(std::string_view app_name, std::string_view submodule) {
  return model_meta_label(app_name, submodule);
}

std::string renamed_method_warning(std::string_view class_name,
                                   std::string_view old_name,
                                   std::string_view new_name) {
  std::string out = "`";
  out.append(class_name);
  out += '.';
  out.append(old_name);
  out += "` is deprecated, use `";
  out.append(class_name);
  out += '.';
  out.append(new_name);
  out += "` instead.";
  return out;
}

bool path_ends_with_py(std::string_view path) noexcept {
  return path.size() >= 3 && path[path.size() - 3] == '.' &&
         path[path.size() - 2] == 'p' && path[path.size() - 1] == 'y';
}

bool path_has_any_suffix(std::string_view path,
                         const std::vector<std::string>& suffixes) {
  for (const auto& s : suffixes) {
    if (s.empty()) {
      continue;
    }
    if (path.size() >= s.size() &&
        path.substr(path.size() - s.size()) == s) {
      return true;
    }
  }
  return false;
}

bool postgres_arrayfield_path_shorten(std::string_view path) noexcept {
  return path == "django.contrib.postgres.fields.array.ArrayField";
}

bool filename_needs_quotes(std::string_view filename) noexcept {
  for (char c : filename) {
    if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == ';' || c == ' ') {
      return true;
    }
  }
  return false;
}

int paginator_num_pages(int count, int per_page, int orphans,
                        bool allow_empty_first_page) noexcept {
  if (per_page <= 0) {
    per_page = 1;
  }
  if (orphans < 0) {
    orphans = 0;
  }
  if (count == 0 && !allow_empty_first_page) {
    return 0;
  }
  int hits = count - orphans;
  if (hits < 1) {
    hits = 1;
  }
  return (hits + per_page - 1) / per_page;  // ceil division
}

int paginator_page_bottom(int number, int per_page) noexcept {
  if (number < 1) {
    number = 1;
  }
  if (per_page < 0) {
    per_page = 0;
  }
  return (number - 1) * per_page;
}

int paginator_page_top(int number, int per_page, int orphans, int count) noexcept {
  int bottom = paginator_page_bottom(number, per_page);
  int top = bottom + per_page;
  if (top + orphans >= count) {
    return count;
  }
  return top;
}

int paginator_number_range_code(int number, int num_pages) noexcept {
  if (number < 1) {
    return 2;
  }
  if (number > num_pages) {
    return 3;
  }
  return 0;
}

bool url_is_relative_path(std::string_view to) noexcept {
  return to.starts_with("./") || to.starts_with("../");
}

bool url_feels_like_url(std::string_view to) noexcept {
  return to.find('/') != std::string_view::npos ||
         to.find('.') != std::string_view::npos;
}

int formset_total_forms_bound(int submitted, int absolute_max) noexcept {
  if (submitted < absolute_max) {
    return submitted;
  }
  return absolute_max;
}

int formset_total_forms_unbound(int initial_forms, int min_num, int extra,
                                int max_num) noexcept {
  int total = initial_forms;
  if (min_num > total) {
    total = min_num;
  }
  total += extra;
  if (initial_forms > max_num && max_num >= 0) {
    return initial_forms;
  }
  if (total > max_num && max_num >= 0) {
    return max_num;
  }
  return total;
}

bool path_has_dotdot(std::string_view path) noexcept {
  // Detect /../ or starting .. or ending /..
  if (path == ".." || path.starts_with("../") || path.ends_with("/..")) {
    return true;
  }
  return path.find("/../") != std::string_view::npos;
}

std::string storage_normalize_name(std::string_view name) {
  std::string out;
  out.reserve(name.size());
  for (char c : name) {
    out += (c == '\\') ? '/' : c;
  }
  return out;
}

std::string storage_alternative_name(std::string_view root, std::string_view random7,
                                     std::string_view ext) {
  std::string out;
  out.reserve(root.size() + random7.size() + ext.size() + 1);
  out.append(root);
  out += '_';
  out.append(random7);
  out.append(ext);
  return out;
}

bool storage_name_available(bool exists, bool has_max_length, int name_len,
                            int max_length) noexcept {
  if (exists) {
    return false;
  }
  if (has_max_length && name_len > max_length) {
    return false;
  }
  return true;
}

bool middleware_capability_ok(bool sync_capable, bool async_capable) noexcept {
  return sync_capable || async_capable;
}

bool sitemap_priority_valid(double priority) noexcept {
  return priority >= 0.0 && priority <= 1.0;
}

bool sitemap_changefreq_valid(std::string_view freq) noexcept {
  return freq == "always" || freq == "hourly" || freq == "daily" ||
         freq == "weekly" || freq == "monthly" || freq == "yearly" ||
         freq == "never";
}

int ordinal_suffix_kind(int value) noexcept {
  if (value < 0) {
    return -1;
  }
  int mod100 = value % 100;
  if (mod100 == 11 || mod100 == 12 || mod100 == 13) {
    return 11;
  }
  return value % 10;
}

std::string intcomma_ascii(std::string_view digits) {
  // digits may include leading sign and optional fractional part
  std::string s(digits);
  std::size_t start = 0;
  std::string sign;
  if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
    sign = s.substr(0, 1);
    start = 1;
  }
  std::size_t dot = s.find('.', start);
  std::string intpart =
      s.substr(start, (dot == std::string::npos ? s.size() : dot) - start);
  std::string frac =
      (dot == std::string::npos) ? std::string() : std::string(s.substr(dot));
  std::string with_commas;
  with_commas.reserve(intpart.size() + intpart.size() / 3);
  int count = 0;
  for (std::size_t i = intpart.size(); i > 0; --i) {
    if (count && count % 3 == 0) {
      with_commas.push_back(',');
    }
    with_commas.push_back(intpart[i - 1]);
    ++count;
  }
  std::reverse(with_commas.begin(), with_commas.end());
  return sign + with_commas + frac;
}

bool check_is_serious(int level, int threshold) noexcept {
  return level >= threshold;
}

std::string path_with_query(std::string_view path, std::string_view query) {
  if (query.empty()) {
    return std::string(path);
  }
  std::string out;
  out.reserve(path.size() + query.size() + 1);
  out.append(path);
  out += '?';
  out.append(query);
  return out;
}

std::string ensure_leading_slash(std::string_view path) {
  if (path.empty()) {
    return "/";
  }
  if (path.front() == '/') {
    return std::string(path);
  }
  std::string out = "/";
  out.append(path);
  return out;
}

bool redirect_paths_equal(std::string_view a, std::string_view b) noexcept {
  return a == b;
}

std::string wkt_point(std::string_view x, std::string_view y) {
  std::string out = "POINT(";
  out.append(x);
  out += ' ';
  out.append(y);
  out += ')';
  return out;
}

std::string postgres_empty_array_literal() {
  return "{}";
}

// --- menu deep: generic views → syndication/test utils -----------------------

std::string list_context_object_name(std::string_view model_name) {
  std::string out;
  out.reserve(model_name.size() + 5);
  out.append(model_name);
  out.append("_list");
  return out;
}

bool http_method_in_names(std::string_view method_lower,
                          const std::vector<std::string>& names) {
  for (const auto& n : names) {
    if (method_lower == n) {
      return true;
    }
  }
  return false;
}

bool page_token_is_last(std::string_view page) noexcept {
  return page == "last";
}

std::string model_template_name(std::string_view app_label,
                                std::string_view object_name,
                                std::string_view suffix) {
  std::string out;
  out.reserve(app_label.size() + object_name.size() + suffix.size() + 6);
  out.append(app_label);
  out += '/';
  out.append(object_name);
  out.append(suffix);
  out.append(".html");
  return out;
}

std::string modelform_class_name(std::string_view model_name) {
  std::string out;
  out.reserve(model_name.size() + 4);
  out.append(model_name);
  out.append("Form");
  return out;
}

bool form_field_included(bool editable, bool fields_is_none, bool in_fields,
                         bool exclude_active, bool in_exclude) noexcept {
  if (!editable) {
    return false;
  }
  if (!fields_is_none && !in_fields) {
    return false;
  }
  if (exclude_active && in_exclude) {
    return false;
  }
  return true;
}

static bool admin_quote_needs(unsigned char c) noexcept {
  // b'":/_#?;@&=+$,"[]<>%\n\\' — quote these as _XX
  switch (c) {
    case '"':
    case ':':
    case '/':
    case '_':
    case '#':
    case '?':
    case ';':
    case '@':
    case '&':
    case '=':
    case '+':
    case '$':
    case ',':
    case '[':
    case ']':
    case '<':
    case '>':
    case '%':
    case '\n':
    case '\\':
      return true;
    default:
      return false;
  }
}

std::string admin_quote(std::string_view s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 2);
  for (unsigned char c : s) {
    if (admin_quote_needs(c)) {
      out += '_';
      out += hex[(c >> 4) & 0xF];
      out += hex[c & 0xF];
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}

bool lookup_key_endswith(std::string_view key,
                         std::string_view suffix) noexcept {
  return key.size() >= suffix.size() &&
         key.substr(key.size() - suffix.size()) == suffix;
}

bool prepare_lookup_isnull(std::string_view value_lower) noexcept {
  return !(value_lower.empty() || value_lower == "false" ||
           value_lower == "0");
}

bool paths_equal(std::string_view a, std::string_view b) noexcept {
  return a == b;
}

bool strings_ci_equal_ascii(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    unsigned char ca = static_cast<unsigned char>(a[i]);
    unsigned char cb = static_cast<unsigned char>(b[i]);
    if (ca >= 'A' && ca <= 'Z') {
      ca = static_cast<unsigned char>(ca - 'A' + 'a');
    }
    if (cb >= 'A' && cb <= 'Z') {
      cb = static_cast<unsigned char>(cb - 'A' + 'a');
    }
    if (ca != cb) {
      return false;
    }
  }
  return true;
}

std::string migration_filename(std::string_view name) {
  std::string out;
  out.reserve(name.size() + 3);
  out.append(name);
  out.append(".py");
  return out;
}

bool introspection_is_table(std::string_view type_code) noexcept {
  return type_code == "t";
}

std::string combined_expression_sql(std::string_view lhs,
                                    std::string_view connector,
                                    std::string_view rhs) {
  // Empty connector → parenthesize lhs only (post combine_expression wrap).
  if (connector.empty() && rhs.empty()) {
    std::string out;
    out.reserve(lhs.size() + 2);
    out += '(';
    out.append(lhs);
    out += ')';
    return out;
  }
  std::string out;
  out.reserve(lhs.size() + connector.size() + rhs.size() + 4);
  out += '(';
  out.append(lhs);
  out += ' ';
  out.append(connector);
  out += ' ';
  out.append(rhs);
  out += ')';
  return out;
}

std::string sql_cast_as_numeric(std::string_view sql) {
  std::string out = "(CAST(";
  out.append(sql);
  out.append(" AS NUMERIC))");
  return out;
}

bool cache_timestamp_expired(bool exp_is_none, double exp, double now) noexcept {
  if (exp_is_none) {
    return false;
  }
  return exp < now;
}

std::string cache_file_name(std::string_view hexdigest,
                            std::string_view suffix) {
  std::string out;
  out.reserve(hexdigest.size() + suffix.size());
  out.append(hexdigest);
  out.append(suffix);
  return out;
}

bool cache_cull_needed(int num_entries, int max_entries) noexcept {
  return num_entries >= max_entries;
}

int cache_cull_sample_size(int num_entries, int cull_frequency) noexcept {
  if (cull_frequency == 0) {
    return 0;  // clear all
  }
  if (cull_frequency < 0) {
    cull_frequency = 1;
  }
  return num_entries / cull_frequency;
}

std::string wsgi_request_path(std::string_view script_name,
                              std::string_view path_info) {
  // Match Python: script_name.rstrip('/') + '/' + path_info.replace('/', '', 1)
  std::string_view sn = script_name;
  while (!sn.empty() && sn.back() == '/') {
    sn.remove_suffix(1);
  }
  std::string pi(path_info);
  if (auto pos = pi.find('/'); pos != std::string::npos) {
    pi.erase(pos, 1);
  }
  std::string out;
  out.reserve(sn.size() + pi.size() + 1);
  out.append(sn);
  out += '/';
  out.append(pi);
  return out;
}

int exception_status_code(std::string_view kind) noexcept {
  if (kind == "http404") {
    return 404;
  }
  if (kind == "permission") {
    return 403;
  }
  if (kind == "bad" || kind == "suspicious" || kind == "multipart") {
    return 400;
  }
  return 500;
}

static bool is_search_spec_char(char c) noexcept {
  // r"['\0\[\]()|&:*!@<>\\]"
  switch (c) {
    case '\'':
    case '\0':
    case '[':
    case ']':
    case '(':
    case ')':
    case '|':
    case '&':
    case ':':
    case '*':
    case '!':
    case '@':
    case '<':
    case '>':
    case '\\':
      return true;
    default:
      return false;
  }
}

std::string postgres_normalize_spaces(std::string_view val) {
  // strip + collapse whitespace runs to single space
  std::size_t start = 0;
  while (start < val.size() &&
         (val[start] == ' ' || val[start] == '\t' || val[start] == '\n' ||
          val[start] == '\r' || val[start] == '\f' || val[start] == '\v')) {
    ++start;
  }
  if (start >= val.size()) {
    return {};
  }
  std::size_t end = val.size();
  while (end > start &&
         (val[end - 1] == ' ' || val[end - 1] == '\t' || val[end - 1] == '\n' ||
          val[end - 1] == '\r' || val[end - 1] == '\f' ||
          val[end - 1] == '\v')) {
    --end;
  }
  std::string out;
  out.reserve(end - start);
  bool prev_space = false;
  for (std::size_t i = start; i < end; ++i) {
    char c = val[i];
    bool sp = (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
               c == '\v');
    if (sp) {
      if (!prev_space) {
        out += ' ';
        prev_space = true;
      }
    } else {
      out += c;
      prev_space = false;
    }
  }
  return out;
}

std::string postgres_psql_escape(std::string_view query) {
  std::string tmp;
  tmp.reserve(query.size());
  for (char c : query) {
    tmp += is_search_spec_char(c) ? ' ' : c;
  }
  return postgres_normalize_spaces(tmp);
}

std::string search_vector_match_sql(std::string_view lhs,
                                    std::string_view rhs) {
  std::string out;
  out.reserve(lhs.size() + rhs.size() + 4);
  out.append(lhs);
  out.append(" @@ ");
  out.append(rhs);
  return out;
}

std::string feed_protocol(bool secure) noexcept {
  return secure ? "https" : "http";
}

bool feed_url_is_network_path(std::string_view url) noexcept {
  return url.starts_with("//");
}

bool feed_url_has_scheme(std::string_view url) noexcept {
  return url.starts_with("http://") || url.starts_with("https://") ||
         url.starts_with("mailto:");
}

std::string feed_network_path_url(std::string_view protocol,
                                  std::string_view url) {
  std::string out;
  out.reserve(protocol.size() + url.size() + 1);
  out.append(protocol);
  out += ':';
  out.append(url);
  return out;
}

std::string feed_absolute_url(std::string_view protocol,
                              std::string_view domain, std::string_view url) {
  // protocol://domain + url  (url may or may not start with /)
  std::string out;
  out.reserve(protocol.size() + domain.size() + url.size() + 3);
  out.append(protocol);
  out.append("://");
  out.append(domain);
  out.append(url);
  return out;
}

std::string dotted_qualname(std::string_view module,
                            std::string_view qualname) {
  std::string out;
  out.reserve(module.size() + qualname.size() + 1);
  out.append(module);
  out += '.';
  out.append(qualname);
  return out;
}

std::string strip_p_tags(std::string_view value) {
  // replace <p> and </p> with empty (case-sensitive, Django style)
  std::string out;
  out.reserve(value.size());
  for (std::size_t i = 0; i < value.size();) {
    if (value.substr(i).starts_with("<p>")) {
      i += 3;
    } else if (value.substr(i).starts_with("</p>")) {
      i += 4;
    } else {
      out += value[i];
      ++i;
    }
  }
  return out;
}

bool approximate_equal(double val, double other, int places) noexcept {
  if (val == other) {
    return true;
  }
  if (places < 0) {
    places = 0;
  }
  double scale = 1.0;
  for (int i = 0; i < places; ++i) {
    scale *= 10.0;
  }
  double diff = val - other;
  if (diff < 0) {
    diff = -diff;
  }
  // Python: round(abs(val - other), places) == 0
  double rounded = std::round(diff * scale) / scale;
  return rounded == 0.0;
}

std::string http_allow_header(const std::vector<std::string>& methods) {
  std::string out;
  for (std::size_t i = 0; i < methods.size(); ++i) {
    if (i) {
      out.append(", ");
    }
    out.append(methods[i]);
  }
  return out;
}

std::string ensure_trailing_slash(std::string_view url) {
  if (url.empty()) {
    return "/";
  }
  if (url.back() == '/') {
    return std::string(url);
  }
  std::string out(url);
  out += '/';
  return out;
}

std::string string_ascii_lower(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if (c >= 'A' && c <= 'Z') {
      out += static_cast<char>(c - 'A' + 'a');
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}

std::string management_command_name(std::string_view path) {
  // basename without .py
  std::size_t slash = path.find_last_of("/\\");
  std::string_view base =
      (slash == std::string_view::npos) ? path : path.substr(slash + 1);
  if (base.size() >= 3 && base.substr(base.size() - 3) == ".py") {
    base.remove_suffix(3);
  }
  return std::string(base);
}

std::string asgi_path_info(std::string_view path, std::string_view script_name) {
  if (script_name.empty()) {
    return std::string(path);
  }
  if (path.starts_with(script_name)) {
    return std::string(path.substr(script_name.size()));
  }
  return std::string(path);
}

// --- menu 1-12 unit-testable footholds --------------------------------------

std::string field_str(std::string_view model_label, std::string_view name) {
  std::string out;
  out.reserve(model_label.size() + name.size() + 1);
  out.append(model_label);
  out += '.';
  out.append(name);
  return out;
}

std::string field_repr(std::string_view path, bool has_name,
                       std::string_view name) {
  if (!has_name) {
    std::string out = "<";
    out.append(path);
    out += '>';
    return out;
  }
  std::string out = "<";
  out.append(path);
  out.append(": ");
  out.append(name);
  out += '>';
  return out;
}

std::string verbose_name_from_name(std::string_view name) {
  std::string out;
  out.reserve(name.size());
  for (char c : name) {
    out += (c == '_') ? ' ' : c;
  }
  return out;
}

int field_name_check_code(std::string_view name) noexcept {
  if (name.empty()) {
    return 0;
  }
  if (name.back() == '_') {
    return 1;
  }
  if (name.find("__") != std::string_view::npos) {
    return 2;
  }
  if (name == "pk") {
    return 3;
  }
  return 0;
}

std::string field_column_name(std::string_view attname,
                              std::string_view db_column) {
  if (!db_column.empty()) {
    return std::string(db_column);
  }
  return std::string(attname);
}

std::string aggregate_default_alias(std::string_view expr_name,
                                    std::string_view agg_name) {
  std::string out;
  out.reserve(expr_name.size() + agg_name.size() + 2);
  out.append(expr_name);
  out.append("__");
  // lower agg name (ASCII)
  for (unsigned char c : agg_name) {
    if (c >= 'A' && c <= 'Z') {
      out += static_cast<char>(c - 'A' + 'a');
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}

std::string sql_distinct_prefix(bool distinct) {
  return distinct ? "DISTINCT " : "";
}

std::string index_column_with_order(std::string_view column, bool descending) {
  if (descending) {
    std::string out = "-";
    out.append(column);
    return out;
  }
  return std::string(column);
}

std::string index_name_fix_leading(std::string_view name) {
  if (name.empty()) {
    return std::string(name);
  }
  unsigned char c0 = static_cast<unsigned char>(name[0]);
  if (c0 == '_' || (c0 >= '0' && c0 <= '9')) {
    std::string out = "D";
    out.append(name.substr(1));
    return out;
  }
  return std::string(name);
}

bool admin_can_show_all(int result_count, int list_max_show_all) noexcept {
  return result_count <= list_max_show_all;
}

bool admin_is_multi_page(int result_count, int list_per_page) noexcept {
  return result_count > list_per_page;
}

std::string query_string_with_prefix(std::string_view encoded) {
  std::string out = "?";
  out.append(encoded);
  return out;
}

std::string css_classes_join(const std::vector<std::string>& classes) {
  std::string out;
  for (std::size_t i = 0; i < classes.size(); ++i) {
    if (i) {
      out += ' ';
    }
    out.append(classes[i]);
  }
  return out;
}

std::string password_reset_token_join(std::string_view ts_b36,
                                      std::string_view hash_hex) {
  std::string out;
  out.reserve(ts_b36.size() + hash_hex.size() + 1);
  out.append(ts_b36);
  out += '-';
  out.append(hash_hex);
  return out;
}

TokenSplit password_reset_token_split(std::string_view token) {
  TokenSplit r{false, {}, {}};
  auto pos = token.find('-');
  if (pos == std::string_view::npos) {
    return r;
  }
  r.ok = true;
  r.ts_b36 = std::string(token.substr(0, pos));
  r.rest = std::string(token.substr(pos + 1));
  return r;
}

bool password_meets_min_length(int password_len, int min_length) noexcept {
  return password_len >= min_length;
}

bool password_is_numeric_only(std::string_view password) noexcept {
  if (password.empty()) {
    return false;
  }
  for (unsigned char c : password) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  return true;
}

std::string migration_node_repr(std::string_view cls, std::string_view app,
                                std::string_view name) {
  // "<%s: (%r, %r)>" with Python-style single-quoted repr for simple strings
  std::string out = "<";
  out.append(cls);
  out.append(": ('");
  out.append(app);
  out.append("', '");
  out.append(name);
  out.append("')>");
  return out;
}

std::string serializer_datetime_import() { return "import datetime"; }

std::string sitemap_absolute_url(std::string_view protocol,
                                 std::string_view domain,
                                 std::string_view path) {
  std::string out;
  out.reserve(protocol.size() + domain.size() + path.size() + 3);
  out.append(protocol);
  out.append("://");
  out.append(domain);
  out.append(path);
  return out;
}

std::string sitemap_paged_url(std::string_view absolute_url, int page) {
  std::string out;
  out.reserve(absolute_url.size() + 16);
  out.append(absolute_url);
  out.append("?p=");
  out.append(std::to_string(page));
  return out;
}

std::string x_robots_tag_value() { return "noindex, noodp, noarchive"; }

bool http_status_session_saveable(int status_code) noexcept {
  return status_code < 500;
}

bool resource_was_modified(bool header_missing, double mtime,
                           double header_mtime) noexcept {
  if (header_missing) {
    return true;
  }
  return mtime > header_mtime;
}

std::string template_register_name(std::string_view explicit_name,
                                   std::string_view func_name) {
  if (!explicit_name.empty()) {
    return std::string(explicit_name);
  }
  return std::string(func_name);
}

std::string normalize_ascii_whitespace(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  bool in_ws = false;
  for (unsigned char c : s) {
    bool ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
               c == '\v');
    if (ws) {
      if (!in_ws) {
        out += ' ';
        in_ws = true;
      }
    } else {
      out += static_cast<char>(c);
      in_ws = false;
    }
  }
  return out;
}

bool html_boolean_attr_is_true(std::string_view name,
                               std::string_view value) noexcept {
  return value.empty() || value == name;
}

std::string sql_func_call(std::string_view function,
                          std::string_view expressions) {
  std::string out;
  out.reserve(function.size() + expressions.size() + 2);
  out.append(function);
  out += '(';
  out.append(expressions);
  out += ')';
  return out;
}

std::string field_display_method_name(std::string_view field_name) {
  std::string out = "get_";
  out.append(field_name);
  out.append("_display");
  return out;
}

bool optimizer_lists_equal_len(int a, int b) noexcept { return a == b; }

// --- Tier A/B unit-testable footholds ----------------------------------------

bool related_name_ends_plus(std::string_view name) noexcept {
  return !name.empty() && name.back() == '+';
}

bool related_name_is_identifier(std::string_view name) noexcept {
  if (name.empty()) {
    return false;
  }
  unsigned char c0 = static_cast<unsigned char>(name[0]);
  if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '_')) {
    return false;
  }
  for (unsigned char c : name) {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '_')) {
      return false;
    }
  }
  return true;
}

bool related_query_name_ends_underscore(std::string_view name) noexcept {
  return !name.empty() && name.back() == '_';
}

bool related_query_name_has_lookup_sep(std::string_view name) noexcept {
  return name.find("__") != std::string_view::npos;
}

std::string fk_default_name(std::string_view model_name,
                            std::string_view pk_name) {
  std::string out;
  out.reserve(model_name.size() + pk_name.size() + 1);
  out.append(model_name);
  out += '_';
  out.append(pk_name);
  return out;
}

std::string related_filter_key(std::string_view field_name,
                               std::string_view rh_field) {
  std::string out;
  out.reserve(field_name.size() + rh_field.size() + 2);
  out.append(field_name);
  out.append("__");
  out.append(rh_field);
  return out;
}

std::string constraint_deconstruct_path(std::string_view path) {
  constexpr std::string_view from = "django.db.models.constraints";
  constexpr std::string_view to = "django.db.models";
  if (path.starts_with(from)) {
    std::string out(to);
    out.append(path.substr(from.size()));
    return out;
  }
  return std::string(path);
}

std::string sql_varchar_type(bool has_max_length, int max_length) {
  if (!has_max_length) {
    return "varchar";
  }
  return "varchar(" + std::to_string(max_length) + ")";
}

std::string sql_decimal_type(int max_digits, int decimal_places) {
  return "numeric(" + std::to_string(max_digits) + ", " +
         std::to_string(decimal_places) + ")";
}

std::string admin_selectfilter_class(bool is_stacked) {
  return is_stacked ? "selectfilterstacked" : "selectfilter";
}

std::string admin_site_repr(std::string_view cls, std::string_view name) {
  std::string out(cls);
  out.append("(name='");
  out.append(name);
  out.append("')");
  return out;
}

std::string permission_str(std::string_view content_type,
                           std::string_view name) {
  std::string out;
  out.reserve(content_type.size() + name.size() + 3);
  out.append(content_type);
  out.append(" | ");
  out.append(name);
  return out;
}

std::string admin_facet_count_key(int index) {
  return std::to_string(index) + "__c";
}

std::string extract_lookup_name(std::string_view lookup) {
  return string_ascii_lower(lookup);
}

std::string sql_now_sqlite() { return "CURRENT_TIMESTAMP"; }

std::string sql_now_postgresql() {
  return "STATEMENT_TIMESTAMP()";
}

std::string feed_tag_uri(std::string_view hostname,
                         std::string_view date_suffix, std::string_view path,
                         std::string_view fragment) {
  // tag:hostname{date}:path/fragment
  std::string out = "tag:";
  out.append(hostname);
  out.append(date_suffix);
  out += ':';
  out.append(path);
  out += '/';
  out.append(fragment);
  return out;
}

int progress_percent(int count, int total) noexcept {
  if (total <= 0) {
    return 0;
  }
  return count * 100 / total;
}

int progress_done_width(int percent, int width) noexcept {
  return percent * width / 100;
}

bool backend_vendor_is(std::string_view vendor,
                       std::string_view expected) noexcept {
  return vendor == expected;
}

std::string management_prog(std::string_view basename,
                            std::string_view subcommand) {
  std::string out;
  out.reserve(basename.size() + subcommand.size() + 1);
  out.append(basename);
  out += ' ';
  out.append(subcommand);
  return out;
}

int filefield_default_max_length() noexcept { return 100; }

std::string jsonfield_internal_type() { return "JSONField"; }

bool test_label_looks_like_path(std::string_view label) noexcept {
  return label.find('/') != std::string_view::npos ||
         label.find('\\') != std::string_view::npos ||
         label.ends_with(".py");
}

std::string debug_template_path(std::string_view name) {
  // technical_500.html style — just return name as relative template
  return std::string(name);
}

bool date_year_in_range(int year) noexcept {
  // datetime.date year range is 1..9999
  return year >= 1 && year <= 9999;
}

std::string unique_constraint_name(std::string_view model,
                                   std::string_view fields_joined) {
  std::string out;
  out.reserve(model.size() + fields_joined.size() + 8);
  out.append(model);
  out.append("_");
  out.append(fields_joined);
  out.append("_uniq");
  return out;
}

bool db_host_is_unix_socket(std::string_view host) noexcept {
  return !host.empty() && host.front() == '/';
}

std::string postgres_set_timezone_sql() {
  return "SELECT set_config('TimeZone', %s, false)";
}

bool mysql_isolation_level_valid(std::string_view level) noexcept {
  // After lowercasing caller
  return level == "read uncommitted" || level == "read committed" ||
         level == "repeatable read" || level == "serializable";
}

static void append_select_cols(std::string& out,
                               const std::vector<std::string>& quoted_cols) {
  for (std::size_t i = 0; i < quoted_cols.size(); ++i) {
    if (i) {
      out.append(", ");
    }
    out.append(quoted_cols[i]);
  }
}

std::string simple_select_eq_limit_sql(std::string_view quoted_table,
                                       const std::vector<std::string>& quoted_cols,
                                       std::string_view quoted_where_col,
                                       int limit) {
  // SELECT c1, c2 FROM t WHERE w = %s LIMIT n
  if (limit < 1) {
    limit = 1;
  }
  std::string out = "SELECT ";
  append_select_cols(out, quoted_cols);
  out.append(" FROM ");
  out.append(quoted_table);
  out.append(" WHERE ");
  out.append(quoted_where_col);
  out.append(" = %s LIMIT ");
  out.append(std::to_string(limit));
  return out;
}

std::string simple_select_all_sql(std::string_view quoted_table,
                                  const std::vector<std::string>& quoted_cols,
                                  int limit) {
  std::string out = "SELECT ";
  append_select_cols(out, quoted_cols);
  out.append(" FROM ");
  out.append(quoted_table);
  if (limit > 0) {
    out.append(" LIMIT ");
    out.append(std::to_string(limit));
  }
  return out;
}

std::string simple_select_in_sql(std::string_view quoted_table,
                                 const std::vector<std::string>& quoted_cols,
                                 std::string_view quoted_where_col,
                                 int n_placeholders) {
  if (n_placeholders < 1) {
    n_placeholders = 1;
  }
  std::string out = "SELECT ";
  append_select_cols(out, quoted_cols);
  out.append(" FROM ");
  out.append(quoted_table);
  out.append(" WHERE ");
  out.append(quoted_where_col);
  out.append(" IN (");
  out.append(sql_in_placeholders(n_placeholders));
  out.append(")");
  return out;
}

}  // namespace django::native
