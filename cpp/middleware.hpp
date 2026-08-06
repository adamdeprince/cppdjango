// Stock Django middleware *bodies* in C++.
//
// Design: Python still *iterates* the middleware chain (cheap). Each stock
// middleware method makes at most one native call so we do not do
// C++→Python→C++ just to walk a list of callables. Custom/user middleware
// stays pure Python.
#pragma once

#include <nanobind/nanobind.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nb = nanobind;

namespace django::native {

// --- SecurityMiddleware ---------------------------------------------------

// SSL redirect: return https URL or nullopt (no redirect).
// exempt_patterns: Python re patterns as strings (ECMAScript); empty = none.
[[nodiscard]] std::optional<std::string> security_process_request(
    bool redirect_enabled, bool is_secure, std::string_view path_lstrip,
    std::string_view full_path, std::string_view redirect_host,
    std::string_view request_host,
    const std::vector<std::string>& exempt_patterns);

// process_response: returns dict
//   {"set": {header: value, ...}, "setdefault": {header: value, ...}}
[[nodiscard]] nb::dict security_process_response(
    bool is_secure, bool has_sts_header, int sts_seconds,
    bool sts_include_subdomains, bool sts_preload, bool content_type_nosniff,
    bool has_content_type_options, nb::handle referrer_policy,
    bool has_referrer_policy, nb::handle cross_origin_opener_policy,
    bool has_coop);

// --- XFrameOptionsMiddleware ----------------------------------------------

// Returns header value to set, or nullopt if middleware should no-op.
[[nodiscard]] std::optional<std::string> xframe_process_response(
    bool already_has_header, bool xframe_options_exempt,
    std::string_view setting_value);

// --- CommonMiddleware -----------------------------------------------------

// Content-Length for non-streaming responses without the header.
// Returns decimal length string or nullopt if no header should be set.
[[nodiscard]] std::optional<std::string> common_content_length_header(
    bool streaming, bool already_has_content_length, std::size_t content_len);

// PREPEND_WWW redirect URL, or empty if host already has www / empty host.
[[nodiscard]] std::optional<std::string> common_www_redirect_url(
    bool prepend_www, std::string_view host, std::string_view scheme,
    std::string_view path);

// --- GZipMiddleware -------------------------------------------------------

// Decision for gzip process_response.
// Returns dict:
//   early_skip: bool       (short body / already encoded — no Vary)
//   set_vary: bool         (patch Vary: Accept-Encoding)
//   should_compress: bool
//   weak_etag: str|None
[[nodiscard]] nb::dict gzip_process_response_plan(
    bool streaming, int content_len, int min_len, bool has_content_encoding,
    std::string_view accept_encoding, std::string_view etag);

// --- ConditionalGetMiddleware ---------------------------------------------

[[nodiscard]] bool conditional_needs_etag(std::string_view cache_control);

// --- SessionMiddleware helpers (status already have http_status_session_saveable)

// Cookie expire plan: returns (max_age: optional int, expires_http_date: optional str)
// max_age_none means browser-close session.
[[nodiscard]] nb::tuple session_cookie_expiry(bool expire_at_browser_close,
                                              int expiry_age_seconds,
                                              double now_unix);

// Session process_response plan (one crossing). Returns dict:
//   action: "noop" | "delete" | "save"
//   need_vary: bool
//   saveable: bool  (only for save)
//   max_age: int|None, expires: str|None
[[nodiscard]] nb::dict session_process_response_plan(
    bool accessed, bool modified, bool empty, bool cookie_in_request,
    bool save_every_request, int status_code, bool expire_at_browser_close,
    int expiry_age_seconds, double now_unix);

// Normalize/validate session key from cookie; empty → invalid.
[[nodiscard]] std::optional<std::string> session_load_key(
    std::string_view cookie_value, int min_length = 8);

// --- CSRF -----------------------------------------------------------------

// Early process_view gate (before origin/referer/token).
// Returns: "done" | "exempt" | "accept" | "check"
[[nodiscard]] std::string csrf_process_view_gate(bool csrf_processing_done,
                                                 bool csrf_exempt,
                                                 std::string_view method,
                                                 bool dont_enforce);

// Safe methods per RFC 9110 as used by Django CSRF.
[[nodiscard]] bool csrf_is_safe_method(std::string_view method) noexcept;

// Token compare after format validation (handles masked token_len secrets).
[[nodiscard]] bool csrf_secrets_match(std::string_view request_token,
                                      std::string_view csrf_secret,
                                      int secret_len, int token_len);

// Origin check (process_view when HTTP_ORIGIN present).
// good_origin: "https://host" or empty if get_host failed.
// exact_origins: CSRF_TRUSTED_ORIGINS without wildcards.
// subdomain_patterns: pairs (scheme, host_pattern) for "*.example.com" style.
[[nodiscard]] bool csrf_origin_verified(
    std::string_view request_origin, std::string_view good_origin,
    const std::vector<std::string>& exact_origins,
    const std::vector<std::pair<std::string, std::string>>& subdomain_patterns);

// Referer check under HTTPS when Origin is absent.
// Returns empty string if ok, else a short reason code:
//   "no_referer" | "malformed" | "insecure" | "bad"
// good_referer: cookie domain or request host (with port when needed).
// trusted_hosts: hosts from CSRF_TRUSTED_ORIGINS (netloc without *).
[[nodiscard]] std::string csrf_check_referer(
    std::string_view referer_header, std::string_view good_referer,
    const std::vector<std::string>& trusted_hosts);

// --- Auth -----------------------------------------------------------------

// LoginRequiredMiddleware process_view gate.
// 0 = skip (not required), 1 = allow (authenticated), 2 = need redirect
[[nodiscard]] int auth_login_required_gate(bool login_required,
                                           bool is_authenticated) noexcept;

// --- Pure-C++ stock chain (only when every middleware is stock-native) ----

// specs: list of dicts { "type": "security"|"xframe"|"common"|"gzip", ...config }
// get_response: Python callable(request) -> response (view layer; may include
// process_view middleware still in Python).
// Runs process_request forward, get_response, process_response reverse.
// Returns response or raises into Python.
[[nodiscard]] nb::object native_stock_chain_call(nb::sequence specs,
                                                 nb::handle request,
                                                 nb::handle get_response);

// True if path is a known fully-native stock middleware class path.
[[nodiscard]] bool is_native_stock_middleware_path(
    std::string_view dotted_path) noexcept;

// --- Hybrid plan (Security/Common/XFrame batched; Session/Auth stay Python) -

// process_request batch: SSL redirect + optional PREPEND_WWW.
// cfg dict keys: security{redirect,redirect_host,exempt_patterns},
//                common{prepend_www} (optional).
// Returns HttpResponsePermanentRedirect or None.
[[nodiscard]] nb::object hybrid_process_request(nb::dict cfg,
                                                nb::handle request);

// process_response batch: X-Frame-Options, Content-Length, security headers.
// Mutates response; returns it. cfg: security{...}, xframe{setting_value},
// common{enabled bool}.
[[nodiscard]] nb::object hybrid_process_response(nb::dict cfg,
                                                 nb::handle request,
                                                 nb::handle response);

// Session process_response: True if a full plan is needed; False = pure no-op
// (not accessed, not modified, not save-every-request).
[[nodiscard]] bool session_response_needs_work(bool accessed, bool modified,
                                               bool save_every_request) noexcept;

}  // namespace django::native
