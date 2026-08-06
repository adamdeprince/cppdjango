#include "middleware.hpp"

#include "cookie.hpp"
#include "orm.hpp"  // hsts, csrf_unmask, session helpers, etc.
#include "request_utils.hpp"  // is_same_domain

#include <chrono>
#include <ctime>
#include <regex>
#include <sstream>
#include <utility>

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

nb::dict session_process_response_plan(bool accessed, bool modified, bool empty,
                                       bool cookie_in_request,
                                       bool save_every_request, int status_code,
                                       bool expire_at_browser_close,
                                       int expiry_age_seconds, double now_unix) {
  nb::dict out;
  out["need_vary"] = false;
  out["saveable"] = false;
  out["max_age"] = nb::none();
  out["expires"] = nb::none();

  if (cookie_in_request && empty) {
    out["action"] = "delete";
    out["need_vary"] = true;
    return out;
  }

  out["need_vary"] = accessed;
  if ((modified || save_every_request) && !empty) {
    out["action"] = "save";
    bool saveable = http_status_session_saveable(status_code);
    out["saveable"] = saveable;
    if (!expire_at_browser_close) {
      auto exp = session_cookie_expiry(false, expiry_age_seconds, now_unix);
      out["max_age"] = exp[0];
      out["expires"] = exp[1];
    }
    // Python sets need_vary when a session cookie is written.
    if (saveable) {
      out["need_vary"] = true;
    }
    return out;
  }
  out["action"] = "noop";
  return out;
}

std::optional<std::string> session_load_key(std::string_view cookie_value,
                                            int min_length) {
  if (session_key_missing(cookie_value)) {
    return std::nullopt;
  }
  if (!is_valid_session_key(cookie_value, min_length, true)) {
    return std::nullopt;
  }
  return std::string(cookie_value);
}

bool csrf_is_safe_method(std::string_view method) noexcept {
  // GET, HEAD, OPTIONS, TRACE
  auto eq = [](std::string_view a, const char* b) {
    if (a.size() != std::char_traits<char>::length(b)) {
      return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
      char c = a[i];
      if (c >= 'a' && c <= 'z') {
        c = static_cast<char>(c - 'a' + 'A');
      }
      if (c != b[i]) {
        return false;
      }
    }
    return true;
  };
  return eq(method, "GET") || eq(method, "HEAD") || eq(method, "OPTIONS") ||
         eq(method, "TRACE");
}

std::string csrf_process_view_gate(bool csrf_processing_done, bool csrf_exempt,
                                   std::string_view method, bool dont_enforce) {
  if (csrf_processing_done) {
    return "done";
  }
  if (csrf_exempt) {
    return "exempt";
  }
  if (csrf_is_safe_method(method) || dont_enforce) {
    return "accept";
  }
  return "check";
}

bool csrf_secrets_match(std::string_view request_token, std::string_view csrf_secret,
                        int secret_len, int token_len) {
  if (secret_len <= 0) {
    return false;
  }
  std::string tok(request_token);
  if (static_cast<int>(tok.size()) == token_len) {
    tok = csrf_unmask_token(tok, secret_len);
  }
  if (static_cast<int>(tok.size()) != secret_len ||
      static_cast<int>(csrf_secret.size()) != secret_len) {
    return false;
  }
  // constant-time compare
  unsigned char diff = 0;
  for (int i = 0; i < secret_len; ++i) {
    diff |= static_cast<unsigned char>(tok[static_cast<std::size_t>(i)]) ^
            static_cast<unsigned char>(csrf_secret[static_cast<std::size_t>(i)]);
  }
  return diff == 0;
}

namespace {

// Minimal scheme://netloc split for Origin/Referer (no full URL parser).
struct OriginParts {
  std::string scheme;
  std::string netloc;
  bool ok = false;
};

OriginParts split_origin_like(std::string_view url) {
  OriginParts out;
  auto scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos || scheme_end == 0) {
    return out;
  }
  out.scheme = std::string(url.substr(0, scheme_end));
  std::string_view rest = url.substr(scheme_end + 3);
  if (rest.empty()) {
    return out;
  }
  // netloc ends at / ? # or end
  std::size_t i = 0;
  while (i < rest.size() && rest[i] != '/' && rest[i] != '?' && rest[i] != '#') {
    ++i;
  }
  if (i == 0) {
    return out;
  }
  out.netloc = std::string(rest.substr(0, i));
  if (out.scheme.empty() || out.netloc.empty()) {
    return out;
  }
  // urlsplit raises ValueError on incomplete IPv6 (e.g. "https://[").
  auto lb = out.netloc.find('[');
  if (lb != std::string::npos) {
    auto rb = out.netloc.find(']', lb);
    if (rb == std::string::npos) {
      return out;  // ok stays false
    }
  }
  out.ok = true;
  return out;
}

}  // namespace

bool csrf_origin_verified(
    std::string_view request_origin, std::string_view good_origin,
    const std::vector<std::string>& exact_origins,
    const std::vector<std::pair<std::string, std::string>>& subdomain_patterns) {
  if (request_origin.empty()) {
    return false;
  }
  if (!good_origin.empty() && request_origin == good_origin) {
    return true;
  }
  for (const auto& exact : exact_origins) {
    if (request_origin == exact) {
      return true;
    }
  }
  auto parsed = split_origin_like(request_origin);
  if (!parsed.ok) {
    return false;
  }
  for (const auto& [scheme, host_pat] : subdomain_patterns) {
    if (scheme == parsed.scheme && is_same_domain(parsed.netloc, host_pat)) {
      return true;
    }
  }
  return false;
}

std::string csrf_check_referer(std::string_view referer_header,
                               std::string_view good_referer,
                               const std::vector<std::string>& trusted_hosts) {
  // Callers treat missing header as no_referer before invoking this.
  // Empty string / non-URL → malformed (matches urlsplit empty scheme/netloc).
  if (referer_header.empty()) {
    return "malformed";
  }
  auto ref = split_origin_like(referer_header);
  if (!ref.ok) {
    return "malformed";
  }
  // Lowercase compare for scheme
  std::string scheme = ref.scheme;
  for (char& c : scheme) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  if (scheme != "https") {
    return "insecure";
  }
  for (const auto& host : trusted_hosts) {
    if (is_same_domain(ref.netloc, host)) {
      return {};
    }
  }
  if (good_referer.empty()) {
    return "bad";
  }
  if (!is_same_domain(ref.netloc, good_referer)) {
    return "bad";
  }
  return {};
}

int auth_login_required_gate(bool login_required,
                             bool is_authenticated) noexcept {
  if (!login_required) {
    return 0;  // skip
  }
  if (is_authenticated) {
    return 1;  // allow
  }
  return 2;  // redirect
}

bool is_native_stock_middleware_path(std::string_view dotted_path) noexcept {
  return dotted_path == "django.middleware.security.SecurityMiddleware" ||
         dotted_path == "django.middleware.clickjacking.XFrameOptionsMiddleware" ||
         dotted_path == "django.middleware.common.CommonMiddleware" ||
         dotted_path == "django.middleware.gzip.GZipMiddleware" ||
         dotted_path == "django.middleware.http.ConditionalGetMiddleware" ||
         dotted_path ==
             "django.contrib.sessions.middleware.SessionMiddleware" ||
         dotted_path ==
             "django.contrib.auth.middleware.AuthenticationMiddleware" ||
         dotted_path == "django.middleware.csrf.CsrfViewMiddleware";
}

namespace {

void apply_security_headers(nb::handle response, const nb::dict& actions) {
  nb::dict set_h = nb::cast<nb::dict>(actions["set"]);
  nb::dict setdef = nb::cast<nb::dict>(actions["setdefault"]);
  for (nb::handle item : set_h.attr("items")()) {
    nb::tuple t = nb::cast<nb::tuple>(item);
    response.attr("headers").attr("__setitem__")(t[0], t[1]);
  }
  for (nb::handle item : setdef.attr("items")()) {
    nb::tuple t = nb::cast<nb::tuple>(item);
    response.attr("headers").attr("setdefault")(t[0], t[1]);
  }
}

}  // namespace

nb::object native_stock_chain_call(nb::sequence specs, nb::handle request,
                                   nb::handle get_response) {
  // specs: list of dicts with "type" and type-specific config.
  // process_request phase (forward). Session/auth process_request that need
  // Python objects (SessionStore, SimpleLazyObject) are applied by the
  // Python wrapper *before* this call; here we only run pure C++ request steps.
  std::vector<nb::dict> spec_list;
  for (nb::handle h : specs) {
    spec_list.push_back(nb::cast<nb::dict>(h));
  }

  auto is_secure = [&]() {
    return nb::cast<bool>(request.attr("is_secure")());
  };
  auto path_lstrip = [&]() {
    return nb::cast<std::string>(
        nb::str(request.attr("path")).attr("lstrip")("/"));
  };
  auto full_path = [&]() {
    return nb::cast<std::string>(request.attr("get_full_path")());
  };
  auto host = [&]() {
    return nb::cast<std::string>(request.attr("get_host")());
  };
  auto has_header = [](nb::handle response, const char* name) {
    return nb::cast<bool>(response.attr("__contains__")(name));
  };

  for (const auto& spec : spec_list) {
    std::string type = nb::cast<std::string>(spec["type"]);
    if (type == "security") {
      std::vector<std::string> pats;
      if (spec.contains("exempt_patterns")) {
        for (nb::handle p : spec["exempt_patterns"]) {
          pats.push_back(nb::cast<std::string>(p));
        }
      }
      auto url = security_process_request(
          nb::cast<bool>(spec["redirect"]), is_secure(), path_lstrip(),
          full_path(), nb::cast<std::string>(spec["redirect_host"]), host(),
          pats);
      if (url) {
        nb::object cls = nb::module_::import_("django.http")
                             .attr("HttpResponsePermanentRedirect");
        return cls(nb::str(url->c_str(), url->size()));
      }
    }
  }

  // View + process_view middleware still behind get_response (Python).
  nb::object response = get_response(request);

  // process_response reverse
  for (auto it = spec_list.rbegin(); it != spec_list.rend(); ++it) {
    const auto& spec = *it;
    std::string type = nb::cast<std::string>(spec["type"]);
    if (type == "security") {
      nb::dict actions = security_process_response(
          is_secure(), has_header(response, "Strict-Transport-Security"),
          nb::cast<int>(spec["sts_seconds"]),
          nb::cast<bool>(spec["sts_include_subdomains"]),
          nb::cast<bool>(spec["sts_preload"]),
          nb::cast<bool>(spec["content_type_nosniff"]),
          has_header(response, "X-Content-Type-Options"),
          spec.contains("referrer_policy") ? nb::handle(spec["referrer_policy"])
                                           : nb::handle(nb::none()),
          has_header(response, "Referrer-Policy"),
          spec.contains("cross_origin_opener_policy")
              ? nb::handle(spec["cross_origin_opener_policy"])
              : nb::handle(nb::none()),
          has_header(response, "Cross-Origin-Opener-Policy"));
      apply_security_headers(response, actions);
    } else if (type == "xframe") {
      bool has = !response.attr("get")("X-Frame-Options").is_none();
      bool exempt = false;
      if (nb::hasattr(response, "xframe_options_exempt")) {
        exempt = nb::cast<bool>(response.attr("xframe_options_exempt"));
      }
      auto val = xframe_process_response(
          has, exempt, nb::cast<std::string>(spec["setting_value"]));
      if (val) {
        response.attr("headers").attr("__setitem__")(
            "X-Frame-Options", nb::str(val->c_str(), val->size()));
      }
    } else if (type == "common") {
      bool streaming = nb::cast<bool>(response.attr("streaming"));
      std::size_t clen = 0;
      if (!streaming) {
        clen = static_cast<std::size_t>(
            nb::len(nb::handle(response.attr("content"))));
      }
      auto cl = common_content_length_header(
          streaming,
          nb::cast<bool>(response.attr("has_header")("Content-Length")), clen);
      if (cl) {
        response.attr("headers").attr("__setitem__")(
            "Content-Length", nb::str(cl->c_str(), cl->size()));
      }
    }
    // gzip still needs Python zlib — not in pure C++ chain body
  }

  return response;
}

nb::object hybrid_process_request(nb::dict cfg, nb::handle request) {
  auto is_secure = [&]() {
    return nb::cast<bool>(request.attr("is_secure")());
  };
  auto path_lstrip = [&]() {
    return nb::cast<std::string>(
        nb::str(request.attr("path")).attr("lstrip")("/"));
  };
  auto full_path = [&]() {
    return nb::cast<std::string>(request.attr("get_full_path")());
  };
  auto host = [&]() {
    return nb::cast<std::string>(request.attr("get_host")());
  };

  // Security SSL redirect
  if (cfg.contains("security")) {
    nb::dict sec = nb::cast<nb::dict>(cfg["security"]);
    std::vector<std::string> pats;
    if (sec.contains("exempt_patterns")) {
      for (nb::handle p : sec["exempt_patterns"]) {
        pats.push_back(nb::cast<std::string>(p));
      }
    }
    auto url = security_process_request(
        nb::cast<bool>(sec["redirect"]), is_secure(), path_lstrip(),
        full_path(), nb::cast<std::string>(sec["redirect_host"]), host(), pats);
    if (url) {
      nb::object cls = nb::module_::import_("django.http")
                           .attr("HttpResponsePermanentRedirect");
      return cls(nb::str(url->c_str(), url->size()));
    }
  }

  // Common PREPEND_WWW (usually off on benches)
  if (cfg.contains("common")) {
    nb::dict common = nb::cast<nb::dict>(cfg["common"]);
    if (common.contains("prepend_www") &&
        nb::cast<bool>(common["prepend_www"])) {
      std::string h = host();
      std::string scheme = nb::cast<std::string>(request.attr("scheme"));
      std::string path = full_path();
      auto url = common_www_redirect_url(true, h, scheme, path);
      if (url) {
        nb::object cls = nb::module_::import_("django.http")
                             .attr("HttpResponsePermanentRedirect");
        return cls(nb::str(url->c_str(), url->size()));
      }
    }
  }

  return nb::none();
}

nb::object hybrid_process_response(nb::dict cfg, nb::handle request,
                                   nb::handle response) {
  auto is_secure = [&]() {
    return nb::cast<bool>(request.attr("is_secure")());
  };
  auto has_header = [](nb::handle resp, const char* name) {
    return nb::cast<bool>(resp.attr("__contains__")(name));
  };

  // X-Frame-Options first (outermost response among header mw)
  if (cfg.contains("xframe")) {
    nb::dict xf = nb::cast<nb::dict>(cfg["xframe"]);
    bool has = !response.attr("get")("X-Frame-Options").is_none();
    bool exempt = false;
    if (nb::hasattr(response, "xframe_options_exempt")) {
      exempt = nb::cast<bool>(response.attr("xframe_options_exempt"));
    }
    auto val = xframe_process_response(
        has, exempt, nb::cast<std::string>(xf["setting_value"]));
    if (val) {
      response.attr("headers").attr("__setitem__")(
          "X-Frame-Options", nb::str(val->c_str(), val->size()));
    }
  }

  // Content-Length
  if (cfg.contains("common")) {
    nb::dict common = nb::cast<nb::dict>(cfg["common"]);
    bool do_cl = true;
    if (common.contains("content_length")) {
      do_cl = nb::cast<bool>(common["content_length"]);
    }
    if (do_cl) {
      bool streaming = nb::cast<bool>(response.attr("streaming"));
      std::size_t clen = 0;
      if (!streaming) {
        clen = static_cast<std::size_t>(
            nb::len(nb::handle(response.attr("content"))));
      }
      auto cl = common_content_length_header(
          streaming,
          nb::cast<bool>(response.attr("has_header")("Content-Length")), clen);
      if (cl) {
        response.attr("headers").attr("__setitem__")(
            "Content-Length", nb::str(cl->c_str(), cl->size()));
      }
    }
  }

  // Security headers
  if (cfg.contains("security")) {
    nb::dict sec = nb::cast<nb::dict>(cfg["security"]);
    nb::dict actions = security_process_response(
        is_secure(), has_header(response, "Strict-Transport-Security"),
        nb::cast<int>(sec["sts_seconds"]),
        nb::cast<bool>(sec["sts_include_subdomains"]),
        nb::cast<bool>(sec["sts_preload"]),
        nb::cast<bool>(sec["content_type_nosniff"]),
        has_header(response, "X-Content-Type-Options"),
        sec.contains("referrer_policy") ? nb::handle(sec["referrer_policy"])
                                        : nb::handle(nb::none()),
        has_header(response, "Referrer-Policy"),
        sec.contains("cross_origin_opener_policy")
            ? nb::handle(sec["cross_origin_opener_policy"])
            : nb::handle(nb::none()),
        has_header(response, "Cross-Origin-Opener-Policy"));
    apply_security_headers(response, actions);
  }

  return nb::borrow(response);
}

bool session_response_needs_work(bool accessed, bool modified,
                                 bool save_every_request) noexcept {
  return accessed || modified || save_every_request;
}

namespace {

// Call view(request, *args, **kwargs) — empty args/kwargs is the hot path.
nb::object hybrid_call_view(nb::handle callback, nb::handle request,
                            nb::handle args, nb::handle kwargs) {
  nb::list pos;
  pos.append(request);
  if (!args.is_none()) {
    for (nb::handle a : args) {
      pos.append(a);
    }
  }
  nb::tuple call_args = nb::tuple(pos);
  PyObject* kw_ptr = nullptr;
  nb::dict empty_kw;
  if (!kwargs.is_none() && PyDict_Check(kwargs.ptr()) &&
      PyDict_GET_SIZE(kwargs.ptr()) > 0) {
    kw_ptr = kwargs.ptr();
  } else {
    kw_ptr = empty_kw.ptr();
  }
  PyObject* result =
      PyObject_Call(callback.ptr(), call_args.ptr(), kw_ptr);
  if (!result) {
    throw nb::python_error();
  }
  return nb::steal<nb::object>(result);
}

// Exact-route table lookup → call view. Returns nullopt if miss.
// Prefers exact_callbacks (path → view) for empty-args routes.
std::optional<nb::object> hybrid_exact_route_view(nb::dict bits,
                                                  nb::handle request) {
  nb::object path_info = request.attr("path_info");
  nb::object callback;
  nb::object args = nb::tuple();
  nb::object kwargs = nb::dict();
  bool found = false;

  if (bits.contains("exact_callbacks") && !bits["exact_callbacks"].is_none()) {
    nb::dict cbs = nb::cast<nb::dict>(bits["exact_callbacks"]);
    if (cbs.contains(path_info)) {
      callback = cbs[path_info];
      found = true;
    }
  }
  if (!found) {
    if (!bits.contains("exact_routes") || bits["exact_routes"].is_none()) {
      return std::nullopt;
    }
    nb::dict table = nb::cast<nb::dict>(bits["exact_routes"]);
    if (!table.contains(path_info)) {
      return std::nullopt;
    }
    nb::tuple entry = nb::cast<nb::tuple>(table[path_info]);
    callback = entry[0];
    args = nb::len(entry) > 1 ? nb::object(entry[1]) : nb::tuple();
    kwargs = nb::len(entry) > 2 ? nb::object(entry[2]) : nb::dict();
    if (bits.contains("set_resolver_match") &&
        nb::cast<bool>(bits["set_resolver_match"])) {
      nb::object url_name =
          nb::len(entry) > 3 ? nb::object(entry[3]) : nb::none();
      nb::object route =
          nb::len(entry) > 4 ? nb::object(entry[4]) : nb::none();
      nb::object RM =
          nb::module_::import_("django.urls.resolvers").attr("ResolverMatch");
      request.attr("resolver_match") =
          RM(callback, args, kwargs, url_name, nb::none(), nb::none(), route);
    } else {
      request.attr("resolver_match") = nb::none();
    }
    found = true;
  } else {
    request.attr("resolver_match") = nb::none();
  }

  // Hot path: Call(view, (request,)) — no kwargs alloc for empty-args routes.
  nb::object response;
  if ((args.is_none() ||
       (PyTuple_Check(args.ptr()) && PyTuple_GET_SIZE(args.ptr()) == 0)) &&
      (kwargs.is_none() ||
       (PyDict_Check(kwargs.ptr()) && PyDict_GET_SIZE(kwargs.ptr()) == 0))) {
    PyObject* t = PyTuple_Pack(1, request.ptr());
    if (!t) {
      throw nb::python_error();
    }
    PyObject* result = PyObject_Call(callback.ptr(), t, nullptr);
    Py_DECREF(t);
    if (!result) {
      throw nb::python_error();
    }
    response = nb::steal<nb::object>(result);
  } else {
    response = hybrid_call_view(callback, request, args, kwargs);
  }
  if (response.is_none() && bits.contains("check_response") &&
      !bits["check_response"].is_none()) {
    bits["check_response"](response, callback);
  }
  return response;
}

}  // namespace

nb::object hybrid_chain_call(nb::dict cfg, nb::dict bits, nb::handle request,
                             nb::handle get_response) {
  // --- Security / Common process_request ---------------------------------
  {
    nb::object early = hybrid_process_request(cfg, request);
    if (!early.is_none()) {
      return early;
    }
  }

  nb::object meta = request.attr("META");
  std::string http_cookie;
  try {
    if (nb::hasattr(meta, "get")) {
      nb::object raw = meta.attr("get")("HTTP_COOKIE", "");
      if (!raw.is_none()) {
        http_cookie = nb::cast<std::string>(nb::str(raw));
      }
    }
  } catch (...) {
    http_cookie.clear();
  }

  // --- Session attach ------------------------------------------------------
  // Cold path (no Cookie header): ColdSession stub — no SessionStore.
  // Warm path: real SessionStore with validated key.
  bool cold_session = false;
  if (!bits["session_store"].is_none()) {
    nb::object store_cls = bits["session_store"];
    std::string sess_name =
        nb::cast<std::string>(nb::str(bits["session_cookie_name"]));
    if (http_cookie.empty()) {
      cold_session = true;
      if (bits.contains("cold_session_factory") &&
          !bits["cold_session_factory"].is_none()) {
        request.attr("session") = bits["cold_session_factory"]();
      } else {
        request.attr("session") = store_cls(nb::none());
      }
    } else {
      nb::object session_key = nb::none();
      auto key = cookie_header_get(http_cookie, sess_name);
      if (key) {
        auto valid = session_load_key(*key, 8);
        if (valid) {
          session_key = nb::str(valid->c_str(), valid->size());
          cold_session = false;
        }
      }
      if (session_key.is_none() &&
          bits.contains("cold_session_factory") &&
          !bits["cold_session_factory"].is_none()) {
        // Cookie present but no/invalid session key — still cold-ish.
        cold_session = true;
        request.attr("session") = bits["cold_session_factory"]();
      } else {
        request.attr("session") = store_cls(session_key);
      }
    }
  }

  // --- CSRF process_request (cookie header only) ---------------------------
  bool has_csrf = nb::cast<bool>(bits["has_csrf"]);
  if (has_csrf) {
    std::string csrf_name =
        nb::cast<std::string>(nb::str(bits["csrf_cookie_name"]));
    if (!http_cookie.empty()) {
      auto secret = cookie_header_get(http_cookie, csrf_name);
      if (secret && !secret->empty()) {
        const int secret_len = 32;
        const int token_len = 64;
        const int n = static_cast<int>(secret->size());
        const bool len_ok = csrf_token_length_ok(n, secret_len, token_len) == 0;
        if (len_ok && csrf_token_chars_valid(*secret)) {
          std::string unmasked = *secret;
          if (n == token_len) {
            unmasked = csrf_unmask_token(*secret, secret_len);
          }
          if (!unmasked.empty()) {
            meta.attr("__setitem__")(
                "CSRF_COOKIE", nb::str(unmasked.c_str(), unmasked.size()));
          }
        } else if (bits.contains("csrf_invalid_cookie") &&
                   !bits["csrf_invalid_cookie"].is_none()) {
          bits["csrf_invalid_cookie"](request);
        }
      }
    }
  }

  // --- Auth attach ---------------------------------------------------------
  // Cold session → eager AnonymousUser (no SimpleLazyObject / get_user).
  // Warm session → SimpleLazyObject(partial(get_user, request)).
  if (nb::cast<bool>(bits["has_auth"])) {
    nb::object partial = nb::module_::import_("functools").attr("partial");
    nb::object auser = bits["auser"];
    if (cold_session && bits.contains("anonymous_user") &&
        !bits["anonymous_user"].is_none()) {
      request.attr("user") = bits["anonymous_user"]();
    } else {
      nb::object SLO = nb::module_::import_("django.utils.functional")
                           .attr("SimpleLazyObject");
      nb::object get_user = bits["get_user"];
      request.attr("user") = SLO(partial(get_user, request));
    }
    request.attr("auser") = partial(auser, request);
  }

  // --- Safe-method CSRF accept + skip view middleware ----------------------
  std::string method = nb::cast<std::string>(nb::str(request.attr("method")));
  bool dont_enforce = false;
  if (nb::hasattr(request, "_dont_enforce_csrf_checks")) {
    dont_enforce = nb::cast<bool>(request.attr("_dont_enforce_csrf_checks"));
  }
  bool safe_csrf = has_csrf && (csrf_is_safe_method(method) || dont_enforce);
  if (safe_csrf) {
    request.attr("csrf_processing_done") = true;
    request.attr("_skip_view_middleware") = true;
  }

  // --- View layer: exact-route call from C++ when possible -----------------
  nb::object response;
  bool used_exact = false;
  if (safe_csrf || !has_csrf) {
    auto exact = hybrid_exact_route_view(bits, request);
    if (exact) {
      response = *exact;
      used_exact = true;
    }
  }
  if (!used_exact) {
    response = get_response(request);
  }

  // --- CSRF process_response only when cookie must be written --------------
  if (has_csrf && bits.contains("csrf_process_response") &&
      !bits["csrf_process_response"].is_none()) {
    bool needs = false;
    try {
      nb::object flag = meta.attr("get")("CSRF_COOKIE_NEEDS_UPDATE", false);
      needs = nb::cast<bool>(flag);
    } catch (...) {
      needs = false;
    }
    if (needs) {
      response = bits["csrf_process_response"](request, response);
    }
  }

  // --- Session process_response --------------------------------------------
  // ColdSession with accessed=False → pure no-op without calling Python mw.
  if (!bits["session_store"].is_none()) {
    bool save_every = false;
    if (bits.contains("save_every_request")) {
      save_every = nb::cast<bool>(bits["save_every_request"]);
    }
    bool accessed = false;
    bool modified = false;
    try {
      accessed = nb::cast<bool>(request.attr("session").attr("accessed"));
      modified = nb::cast<bool>(request.attr("session").attr("modified"));
    } catch (...) {
      accessed = true;  // force Python path on error
    }
    if (session_response_needs_work(accessed, modified, save_every)) {
      if (bits.contains("session_process_response") &&
          !bits["session_process_response"].is_none()) {
        response = bits["session_process_response"](request, response);
      }
    }
  }

  // --- Header middleware batch ---------------------------------------------
  return hybrid_process_response(cfg, request, response);
}

}  // namespace django::native
