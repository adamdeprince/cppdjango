#include "datastructures.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace django::native {

MvdLastResult mvd_last_value(const std::vector<std::string>& values) {
  MvdLastResult r;
  if (values.empty()) {
    r.empty_list = true;
    return r;
  }
  r.last = values.back();
  return r;
}

int node_add_action(std::string_view self_connector, std::string_view conn_type,
                    bool data_is_node, bool data_negated,
                    std::string_view data_connector, int data_len) {
  if (self_connector != conn_type) {
    return 0;  // nest
  }
  if (data_is_node && !data_negated &&
      (data_connector == conn_type || data_len == 1)) {
    return 1;  // squash
  }
  return 2;  // append
}

std::string form_add_prefix(std::string_view prefix, std::string_view field_name) {
  if (prefix.empty()) {
    return std::string(field_name);
  }
  std::string out;
  out.reserve(prefix.size() + 1 + field_name.size());
  out.append(prefix);
  out += '-';
  out.append(field_name);
  return out;
}

std::string form_add_initial_prefix(std::string_view prefix,
                                    std::string_view field_name) {
  std::string inner = form_add_prefix(prefix, field_name);
  return "initial-" + inner;
}

std::string pretty_name(std::string_view name) {
  if (name.empty()) {
    return {};
  }
  std::string out;
  out.reserve(name.size());
  for (char c : name) {
    out += (c == '_') ? ' ' : c;
  }
  // capitalize: first char upper (ASCII), rest unchanged (str.capitalize)
  if (!out.empty() && out[0] >= 'a' && out[0] <= 'z') {
    out[0] = static_cast<char>(out[0] - 'a' + 'A');
  }
  for (std::size_t i = 1; i < out.size(); ++i) {
    if (out[i] >= 'A' && out[i] <= 'Z') {
      out[i] = static_cast<char>(out[i] - 'A' + 'a');
    }
  }
  return out;
}

std::string form_auto_id(std::string_view auto_id, std::string_view html_name) {
  if (auto_id.empty()) {
    return {};
  }
  // Python: if auto_id and "%s" in str(auto_id): return auto_id % html_name
  if (auto_id.find("%s") != std::string_view::npos) {
    std::string out;
    std::size_t pos = 0;
    while (pos < auto_id.size()) {
      auto found = auto_id.find("%s", pos);
      if (found == std::string_view::npos) {
        out.append(auto_id.substr(pos));
        break;
      }
      out.append(auto_id.substr(pos, found - pos));
      out.append(html_name);
      pos = found + 2;
    }
    return out;
  }
  // elif auto_id: return html_name  (truthy non-format)
  // Treat "False"/"0"/"None" as empty? Python Form default is "id_%s".
  // Boolean False would be falsy — str not used. If someone passes False,
  // Python property gets False and `if auto_id` fails.
  // We only get strings from Python wrapper.
  return std::string(html_name);
}

bool checkbox_bool_value(bool key_present, std::string_view value) {
  // CheckboxInput.value_from_datadict:
  //   missing → False
  //   str: map true/false (case-insensitive); else bool(value) ("" → False)
  if (!key_present) {
    return false;
  }
  std::string lower;
  lower.reserve(value.size());
  for (char c : value) {
    if (c >= 'A' && c <= 'Z') {
      lower += static_cast<char>(c - 'A' + 'a');
    } else {
      lower += c;
    }
  }
  if (lower == "false") {
    return false;
  }
  if (lower == "true") {
    return true;
  }
  return !value.empty();
}

std::string flatatt_build(
    const std::vector<std::pair<std::string, std::string>>& key_values,
    const std::vector<std::string>& boolean_keys) {
  // Sorted key=value attrs then boolean keys (Django: sorted key_value + boolean)
  auto kvs = key_values;
  std::sort(kvs.begin(), kvs.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  auto bools = boolean_keys;
  std::sort(bools.begin(), bools.end());
  std::string out;
  for (const auto& [k, v] : kvs) {
    out += ' ';
    out += k;
    out += "=\"";
    out += v;
    out += '"';
  }
  for (const auto& k : bools) {
    out += ' ';
    out += k;
  }
  return out;
}

std::string json_script_escape(std::string_view json_str) {
  std::string out;
  out.reserve(json_str.size() + 8);
  for (char c : json_str) {
    switch (c) {
      case '>':
        out += "\\u003E";
        break;
      case '<':
        out += "\\u003C";
        break;
      case '&':
        out += "\\u0026";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

std::string json_script_wrap(std::string_view escaped_json,
                             std::string_view element_id) {
  // Caller must ensure element_id is safe / already escaped for HTML attr.
  std::string out;
  if (!element_id.empty()) {
    out = "<script id=\"";
    out.append(element_id);
    out += "\" type=\"application/json\">";
  } else {
    out = "<script type=\"application/json\">";
  }
  out.append(escaped_json);
  out += "</script>";
  return out;
}

std::optional<std::string> floatformat_simple(std::string_view decimal_str, int p) {
  // Very small subset: plain number string, no locale, |p| reasonable.
  if (decimal_str.empty() || decimal_str.size() > 200) {
    return std::nullopt;
  }
  // Reject non-ASCII / exponent
  for (char c : decimal_str) {
    if (!((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+')) {
      return std::nullopt;
    }
  }
  char* end = nullptr;
  std::string tmp(decimal_str);
  double v = std::strtod(tmp.c_str(), &end);
  if (end == tmp.c_str() || *end != '\0') {
    return std::nullopt;
  }
  if (!std::isfinite(v)) {
    return std::nullopt;
  }
  // Integer path when p <= 0 and value is whole
  double intpart = 0;
  double frac = std::modf(v, &intpart);
  if (p <= 0 && std::fabs(frac) < 1e-12) {
    // format as integer
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.0f", intpart);
    return std::string(buf);
  }
  int prec = std::abs(p);
  if (prec > 20) {
    return std::nullopt;
  }
  // When p < 0, drop trailing zeros? Django uses Decimal quantize — skip complex
  if (p < 0) {
    return std::nullopt;  // let Python handle optional decimals
  }
  char fmt[16];
  std::snprintf(fmt, sizeof(fmt), "%%.%df", prec);
  char buf[128];
  std::snprintf(buf, sizeof(buf), fmt, v);
  return std::string(buf);
}

}  // namespace django::native
