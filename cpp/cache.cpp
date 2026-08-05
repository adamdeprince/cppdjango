#include "cache.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

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

[[nodiscard]] std::string_view strip_ws(std::string_view s) {
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

[[nodiscard]] std::string strip_weak(std::string_view etag) {
  std::string s(etag);
  // strip("W/") repeatedly equivalent for leading W/
  while (s.size() >= 2 && (s[0] == 'W' || s[0] == 'w') && s[1] == '/') {
    s.erase(0, 2);
  }
  // Python str.strip("W/") strips any of those chars from both ends
  auto is_ws_set = [](char c) {
    return c == 'W' || c == '/' || c == 'w';  // strip is case-sensitive actually
  };
  // Python: "W/\"abc\"".strip("W/") — strip chars in set {W, /} from ends
  auto strip_set = [](std::string& x) {
    auto bad = [](char c) { return c == 'W' || c == '/'; };
    while (!x.empty() && bad(x.front())) {
      x.erase(x.begin());
    }
    while (!x.empty() && bad(x.back())) {
      x.pop_back();
    }
  };
  strip_set(s);
  return s;
}

}  // namespace

std::vector<std::string> cc_delim_split(std::string_view header) {
  std::vector<std::string> out;
  if (header.empty()) {
    return out;
  }
  std::size_t i = 0;
  while (i < header.size()) {
    // skip leading ws
    while (i < header.size() &&
           (header[i] == ' ' || header[i] == '\t' || header[i] == '\n' ||
            header[i] == '\r')) {
      ++i;
    }
    if (i >= header.size()) {
      break;
    }
    std::size_t start = i;
    while (i < header.size() && header[i] != ',') {
      ++i;
    }
    std::size_t end = i;
    // trim trailing ws of token
    while (end > start &&
           (header[end - 1] == ' ' || header[end - 1] == '\t' || header[end - 1] == '\n' ||
            header[end - 1] == '\r')) {
      --end;
    }
    if (end > start) {
      out.emplace_back(header.substr(start, end - start));
    }
    if (i < header.size() && header[i] == ',') {
      ++i;
      // skip ws after comma (part of \\s*,\\s*)
      while (i < header.size() &&
             (header[i] == ' ' || header[i] == '\t' || header[i] == '\n' ||
              header[i] == '\r')) {
        ++i;
      }
    }
  }
  return out;
}

std::vector<std::pair<std::string, std::string>> parse_cache_control(
    std::string_view header) {
  std::vector<std::pair<std::string, std::string>> out;
  for (const auto& field : cc_delim_split(header)) {
    auto eq = field.find('=');
    if (eq == std::string::npos) {
      out.emplace_back(ascii_lower(field), "");
    } else {
      out.emplace_back(ascii_lower(std::string_view(field).substr(0, eq)),
                       field.substr(eq + 1));
    }
  }
  return out;
}

std::optional<int> get_max_age_from_cc(std::string_view header) {
  for (const auto& [dir, val] : parse_cache_control(header)) {
    if (dir == "max-age" && !val.empty()) {
      try {
        return std::stoi(val);
      } catch (...) {
        return std::nullopt;
      }
    }
  }
  return std::nullopt;
}

bool if_match_passes(std::string_view target_etag,
                     const std::vector<std::string>& etags) {
  if (target_etag.empty()) {
    return false;
  }
  if (etags.size() == 1 && etags[0] == "*") {
    return true;
  }
  if (target_etag.size() >= 2 && target_etag[0] == 'W' && target_etag[1] == '/') {
    return false;
  }
  for (const auto& e : etags) {
    if (e == target_etag) {
      return true;
    }
  }
  return false;
}

bool if_none_match_passes(std::string_view target_etag,
                          const std::vector<std::string>& etags) {
  if (target_etag.empty()) {
    return true;
  }
  if (etags.size() == 1 && etags[0] == "*") {
    return false;
  }
  std::string target = strip_weak(target_etag);
  for (const auto& e : etags) {
    if (strip_weak(e) == target) {
      return false;  // matched → If-None-Match fails (resource matches)
    }
  }
  return true;
}

bool if_unmodified_since_passes(std::optional<std::int64_t> last_modified,
                                std::int64_t if_unmodified_since) {
  return last_modified.has_value() && *last_modified <= if_unmodified_since;
}

bool if_modified_since_passes(std::optional<std::int64_t> last_modified,
                              std::int64_t if_modified_since) {
  return !last_modified.has_value() || *last_modified > if_modified_since;
}

std::string patch_vary_headers(std::string_view existing_vary,
                               const std::vector<std::string>& newheaders) {
  std::vector<std::string> vary_headers;
  if (!existing_vary.empty()) {
    vary_headers = cc_delim_split(existing_vary);
  }
  std::vector<std::string> existing_lower;
  existing_lower.reserve(vary_headers.size());
  for (const auto& h : vary_headers) {
    existing_lower.push_back(ascii_lower(h));
  }
  for (const auto& nh : newheaders) {
    std::string low = ascii_lower(nh);
    bool found = false;
    for (const auto& e : existing_lower) {
      if (e == low) {
        found = true;
        break;
      }
    }
    if (!found) {
      vary_headers.push_back(nh);
      existing_lower.push_back(std::move(low));
    }
  }
  for (const auto& h : vary_headers) {
    if (h == "*") {
      return "*";
    }
  }
  std::string out;
  for (std::size_t i = 0; i < vary_headers.size(); ++i) {
    if (i) {
      out += ", ";
    }
    out += vary_headers[i];
  }
  return out;
}

bool has_vary_header(std::string_view vary_header, std::string_view header_query) {
  if (vary_header.empty()) {
    return false;
  }
  std::string q = ascii_lower(strip_ws(header_query));
  for (const auto& h : cc_delim_split(vary_header)) {
    if (ascii_lower(strip_ws(h)) == q) {
      return true;
    }
  }
  return false;
}

std::string merge_cache_control(
    std::string_view existing_header,
    const std::vector<std::tuple<std::string, std::string, bool>>& kwargs) {
  // Map directive → either string value, or set of values for no-cache.
  // Use vector of pairs + special handling for no-cache multi.
  struct Entry {
    bool is_set = false;
    std::string value;               // when !is_set
    std::vector<std::string> values; // when is_set (no-cache); empty string = True
  };
  std::vector<std::pair<std::string, Entry>> order;
  auto find_or_add = [&](const std::string& dir) -> Entry& {
    for (auto& p : order) {
      if (p.first == dir) {
        return p.second;
      }
    }
    order.emplace_back(dir, Entry{});
    return order.back().second;
  };

  for (const auto& field : cc_delim_split(existing_header)) {
    auto eq = field.find('=');
    std::string dir;
    std::string val;
    bool is_true = false;
    if (eq == std::string::npos) {
      dir = ascii_lower(field);
      is_true = true;
    } else {
      dir = ascii_lower(std::string_view(field).substr(0, eq));
      val = field.substr(eq + 1);
    }
    auto& e = find_or_add(dir);
    if (dir == "no-cache") {
      e.is_set = true;
      e.values.push_back(is_true ? std::string{} : val);
      // mark True as empty; use sentinel — store "\x01" for True?
      if (is_true) {
        e.values.back() = "\x01";  // internal True marker
      }
    } else {
      e.is_set = false;
      e.value = is_true ? std::string{} : val;
      if (is_true) {
        e.value = "\x01";
      }
    }
  }

  // kwargs
  for (const auto& [k, v, is_bool_true] : kwargs) {
    std::string dir;
    dir.reserve(k.size());
    for (char c : k) {
      dir += (c == '_') ? '-' : static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    }
    // max-age min
    if (dir == "max-age") {
      auto& e = find_or_add(dir);
      if (!e.value.empty() && e.value != "\x01") {
        try {
          int existing = std::stoi(e.value);
          int incoming = std::stoi(v);
          e.value = std::to_string(std::min(existing, incoming));
          e.is_set = false;
          continue;
        } catch (...) {
        }
      }
    }
    // private/public mutual exclusion
    if (dir == "public") {
      order.erase(std::remove_if(order.begin(), order.end(),
                                 [](const auto& p) { return p.first == "private"; }),
                  order.end());
    } else if (dir == "private") {
      order.erase(std::remove_if(order.begin(), order.end(),
                                 [](const auto& p) { return p.first == "public"; }),
                  order.end());
    }
    auto& e = find_or_add(dir);
    if (dir == "no-cache") {
      e.is_set = true;
      e.values.push_back(is_bool_true ? std::string("\x01") : v);
    } else {
      e.is_set = false;
      e.value = is_bool_true ? std::string("\x01") : v;
    }
  }

  auto dictvalue = [](const std::string& dir, const std::string& val) {
    if (val == "\x01" || val.empty()) {
      // empty non-marker shouldn't happen for bool; True → name only
      if (val == "\x01") {
        return dir;
      }
    }
    if (val == "\x01") {
      return dir;
    }
    return dir + "=" + val;
  };

  std::vector<std::string> directives;
  for (auto& [dir, e] : order) {
    if (e.is_set) {
      bool has_true = false;
      for (const auto& v : e.values) {
        if (v == "\x01") {
          has_true = true;
          break;
        }
      }
      if (has_true) {
        directives.push_back(dir);
      } else {
        for (const auto& v : e.values) {
          directives.push_back(dictvalue(dir, v));
        }
      }
    } else {
      directives.push_back(dictvalue(dir, e.value));
    }
  }
  std::string out;
  for (std::size_t i = 0; i < directives.size(); ++i) {
    if (i) {
      out += ", ";
    }
    out += directives[i];
  }
  return out;
}

}  // namespace django::native
