// MultiValueDict / Node tree / forms naming helpers.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace django::native {

// --- MultiValueDict ---------------------------------------------------------

// Last element of a values list (MVD.__getitem__). empty_list → return empty
// optional meaning "return []" in Python; nullopt means key missing is N/A here.
// We only handle the list-side: given N string values, return last (or mark empty).
struct MvdLastResult {
  bool empty_list = false;  // true → Python should return []
  std::string last;         // valid when !empty_list
};
[[nodiscard]] MvdLastResult mvd_last_value(const std::vector<std::string>& values);

// --- Node / Q tree (django.utils.tree.Node.add) ------------------------------

// Decision for Node.add:
//   0 → connector differs: copy self, set connector, children=[copy, data]
//   1 → squash: extend self.children with data.children
//   2 → append: self.children.append(data)
[[nodiscard]] int node_add_action(std::string_view self_connector,
                                  std::string_view conn_type, bool data_is_node,
                                  bool data_negated, std::string_view data_connector,
                                  int data_len);

// --- Forms naming -----------------------------------------------------------

[[nodiscard]] std::string form_add_prefix(std::string_view prefix,
                                          std::string_view field_name);
[[nodiscard]] std::string form_add_initial_prefix(std::string_view prefix,
                                                  std::string_view field_name);
[[nodiscard]] std::string pretty_name(std::string_view name);

// auto_id property: auto_id may be bool-like ("True"/nonempty) or format with %s.
// Returns empty if auto_id is empty/"False"/"0".
[[nodiscard]] std::string form_auto_id(std::string_view auto_id,
                                       std::string_view html_name);

// CheckboxInput.value_from_datadict: false_values = {"false","0",...} when present.
// key_present false → return false; value in false_values → false; else true.
[[nodiscard]] bool checkbox_bool_value(bool key_present, std::string_view value);

// flatatt: pairs of (key, value_string, is_bool). bool false skipped; bool true
// bare key; else key="escaped_value". Values are already HTML-escaped by caller
// when needed — we only structure. Returns leading-space attrs string without
// mark_safe (Python wraps).
// Actually Django flatatt escapes via format_html_join. We emit unescaped structure
// for ASCII keys and pre-escaped values from Python... Keep simple: keys sorted,
// format key="value" with caller-escaped values.
[[nodiscard]] std::string flatatt_build(
    const std::vector<std::pair<std::string, std::string>>& key_values,
    const std::vector<std::string>& boolean_keys);

// json_script: escape < > & in JSON text; wrap in script tag.
[[nodiscard]] std::string json_script_escape(std::string_view json_str);
[[nodiscard]] std::string json_script_wrap(std::string_view escaped_json,
                                           std::string_view element_id);

// floatformat core for unlocalized ASCII decimals when use_l10n=false and no
// grouping. input is Decimal string (sign, digits, optional . frac).
// arg_p is the integer precision (already stripped of g/u).
// Returns nullopt when caller should use Python (inf/nan/too large/grouping).
[[nodiscard]] std::optional<std::string> floatformat_simple(std::string_view decimal_str,
                                                            int p);

}  // namespace django::native
