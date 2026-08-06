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

}  // namespace django::native
