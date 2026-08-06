// Helpers for building Python containers at the nanobind boundary without
// going through stl/vector casters (vector of strings → temporary list caster
// would re-copy into a second Python list structure).
#pragma once

#include <nanobind/nanobind.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nb = nanobind;

namespace django::native {

inline nb::list list_from_strings(const std::vector<std::string>& v) {
  nb::list out;
  for (const auto& s : v) {
    out.append(nb::str(s.c_str(), s.size()));
  }
  return out;
}

inline nb::list list_from_strings(std::vector<std::string>&& v) {
  nb::list out;
  for (auto& s : v) {
    out.append(nb::str(s.c_str(), s.size()));
  }
  return out;
}

inline nb::list list_from_string_pairs(
    const std::vector<std::pair<std::string, std::string>>& v) {
  nb::list out;
  for (const auto& p : v) {
    out.append(nb::make_tuple(nb::str(p.first.c_str(), p.first.size()),
                              nb::str(p.second.c_str(), p.second.size())));
  }
  return out;
}

inline nb::list list_from_i64_string_pairs(
    const std::vector<std::pair<std::int64_t, std::string>>& v) {
  nb::list out;
  for (const auto& p : v) {
    out.append(nb::make_tuple(nb::int_(p.first),
                              nb::str(p.second.c_str(), p.second.size())));
  }
  return out;
}

inline nb::list list_from_int_int_pairs(
    const std::vector<std::pair<int, int>>& v) {
  nb::list out;
  for (const auto& p : v) {
    out.append(nb::make_tuple(nb::int_(p.first), nb::int_(p.second)));
  }
  return out;
}

}  // namespace django::native
