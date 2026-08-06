#include "middleware.hpp"

#include "orm.hpp"  // hsts_header_value, https_redirect_url, referrer_policy, xframe, gzip helpers

#include <chrono>
#include <ctime>
#include <regex>
#include <sstream>

namespace django::native {
namespace {

bool host_has_www(std::string_view host) {
  return host.size() >= 4 &&
         (host[0] == 'w' || host[0] == 'W') &&
         (host[1] == 'w' || host[1] == 'W') &&
         (host[2] == 'w' || host[2] == 'W') && host[3] == '.';
}

// HTTP-date (IMF-fix) for Set-Cookie expires — matches django.utils.http.http_date.
std::string http_date_from_unix(double unix_ts) {
  std::time_t t = static_cast<std::time_t>(unix_ts);
  std::tm gmt{};
#if defined(_WIN32)
  gmtime_s(&gmt, &t);
#else
  gmtime_r(&t, &gmt);
#endif
  char buf[64];
  // Example: Wed, 21 Oct 2015 07:28:00 GMT
  if (std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &gmt) == 0) {
    return {};
  }
  return std::string(buf);
}

bool path_matches_any(std::string_view path,
                      const std::vector<std::string>& patterns) {
  for (const auto& p : patterns) {
    if (p.empty()) {
      continue;
    }
    try {
      std::regex re(p, std::regex::ECMAScript | std::regex::optimize);
      if (std::regex_search(path.begin(), path.end(), re)) {
        return true;
      }
    } catch (const std::regex_error&) {
      // Invalid pattern: ignore (Python would have failed at settings load).
    }
  }
  return false;
}

}  // namespace

std::optional<std::string> security_process_request(
    bool redirect_enabled, bool is_secure, std::string_view path_lstrip,
    std::string_view full_path, std::string_view redirect_host,
    std::string_view request_host,
    const std::vector<std::string>& exempt_patterns) {
  if (!redirect_enabled || is_secure) {
    return std::nullopt;
  }
  if (path_matches_any(path_lstrip, exempt_patterns)) {
    return std::nullopt;
  }
  std::string_view host =
      !redirect_host.empty() ? redirect_host : request_host;
  if (host.empty()) {
    return std::nullopt;
  }
  return https_redirect_url(host, full_path);
}

nb::dict security_process_response(
    bool is_secure, bool has_sts_header, int sts_seconds,
    bool sts_include_subdomains, bool sts_preload, bool content_type_nosniff,
    bool has_content_type_options, nb::handle referrer_policy,
    bool has_referrer_policy, nb::handle cross_origin_opener_policy,
    bool has_coop) {
  nb::dict set_headers;
  nb::dict setdefault_headers;

  auto put_str = [](nb::dict& d, const char* key, const std::string& val) {
    d[key] = nb::str(val.c_str(), val.size());
  };

  if (sts_seconds > 0 && is_secure && !has_sts_header) {
    put_str(set_headers, "Strict-Transport-Security",
            hsts_header_value(sts_seconds, sts_include_subdomains, sts_preload));
  }

  if (content_type_nosniff && !has_content_type_options) {
    put_str(setdefault_headers, "X-Content-Type-Options", "nosniff");
  }

  if (!referrer_policy.is_none() && !has_referrer_policy) {
    std::vector<std::string> parts;
    if (nb::isinstance<nb::str>(referrer_policy)) {
      std::string raw = nb::cast<std::string>(referrer_policy);
      // split on comma
      std::size_t start = 0;
      while (start < raw.size()) {
        auto comma = raw.find(',', start);
        std::string piece = raw.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start);
        // trim
        std::size_t a = 0;
        while (a < piece.size() && (piece[a] == ' ' || piece[a] == '\t')) {
          ++a;
        }
        std::size_t b = piece.size();
        while (b > a && (piece[b - 1] == ' ' || piece[b - 1] == '\t')) {
          --b;
        }
        if (b > a) {
          parts.push_back(piece.substr(a, b - a));
        }
        if (comma == std::string::npos) {
          break;
        }
        start = comma + 1;
      }
    } else {
      for (nb::handle h : referrer_policy) {
        parts.push_back(nb::cast<std::string>(nb::str(h)));
      }
    }
    if (!parts.empty()) {
      put_str(setdefault_headers, "Referrer-Policy",
              referrer_policy_header(parts));
    }
  }

  if (!cross_origin_opener_policy.is_none() && !has_coop) {
    std::string coop = nb::cast<std::string>(nb::str(cross_origin_opener_policy));
    if (!coop.empty()) {
      put_str(setdefault_headers, "Cross-Origin-Opener-Policy", coop);
    }
  }

  nb::dict out;
  out["set"] = set_headers;
  out["setdefault"] = setdefault_headers;
  return out;
}

std::optional<std::string> xframe_process_response(bool already_has_header,
                                                   bool xframe_options_exempt,
                                                   std::string_view setting_value) {
  if (already_has_header || xframe_options_exempt) {
    return std::nullopt;
  }
  return xframe_options_value(setting_value.empty() ? "DENY" : setting_value);
}

std::optional<std::string> common_content_length_header(
    bool streaming, bool already_has_content_length, std::size_t content_len) {
  if (streaming || already_has_content_length) {
    return std::nullopt;
  }
  return std::to_string(content_len);
}

std::optional<std::string> common_www_redirect_url(bool prepend_www,
                                                   std::string_view host,
                                                   std::string_view scheme,
                                                   std::string_view path) {
  if (!prepend_www || host.empty() || host_has_www(host)) {
    return std::nullopt;
  }
  std::string out;
  out.reserve(scheme.size() + host.size() + path.size() + 16);
  out.append(scheme);
  out += "://www.";
  out.append(host);
  out.append(path);
  return out;
}

nb::dict gzip_process_response_plan(bool streaming, int content_len, int min_len,
                                    bool has_content_encoding,
                                    std::string_view accept_encoding,
                                    std::string_view etag) {
  // Mirrors stock GZipMiddleware ordering:
  // short / already-encoded → full skip (no Vary)
  // else patch Vary, then maybe skip compress if Accept-Encoding lacks gzip
  nb::dict out;
  out["early_skip"] = false;
  out["set_vary"] = false;
  out["should_compress"] = false;
  out["weak_etag"] = nb::none();

  if (!streaming && gzip_content_too_short(content_len, min_len)) {
    out["early_skip"] = true;
    return out;
  }
  if (has_content_encoding) {
    out["early_skip"] = true;
    return out;
  }
  out["set_vary"] = true;
  if (!accepts_gzip(accept_encoding)) {
    return out;
  }
  out["should_compress"] = true;
  if (!etag.empty()) {
    std::string weak = weak_etag_if_strong(etag);
    out["weak_etag"] = nb::str(weak.c_str(), weak.size());
  }
  return out;
}

bool conditional_needs_etag(std::string_view cache_control) {
  // Split on comma, reject if any token is no-store (case-insensitive).
  std::size_t start = 0;
  while (start <= cache_control.size()) {
    auto comma = cache_control.find(',', start);
    std::string_view tok = cache_control.substr(
        start, comma == std::string_view::npos ? std::string_view::npos
                                               : comma - start);
    // trim
    while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) {
      tok.remove_prefix(1);
    }
    while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) {
      tok.remove_suffix(1);
    }
    if (tok.size() == 8) {
      bool match = true;
      const char* lit = "no-store";
      for (int i = 0; i < 8; ++i) {
        char c = tok[static_cast<std::size_t>(i)];
        if (c >= 'A' && c <= 'Z') {
          c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != lit[i]) {
          match = false;
          break;
        }
      }
      if (match) {
        return false;
      }
    }
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  return true;
}

nb::tuple session_cookie_expiry(bool expire_at_browser_close, int expiry_age_seconds,
                                double now_unix) {
  if (expire_at_browser_close) {
    return nb::make_tuple(nb::none(), nb::none());
  }
  double exp = now_unix + static_cast<double>(expiry_age_seconds);
  std::string expires = http_date_from_unix(exp);
  return nb::make_tuple(nb::int_(expiry_age_seconds),
                        nb::str(expires.c_str(), expires.size()));
}

}  // namespace django::native
