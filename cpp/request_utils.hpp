// Remaining request-path utilities (django.utils.http + host validation).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace django::native {

// HTTP-date → Unix timestamp (UTC seconds). nullopt if invalid.
// current_year is used for RFC 850 2-digit year windowing.
[[nodiscard]] std::optional<std::int64_t> parse_http_date(std::string_view date,
                                                          int current_year);

// Unix timestamp → "Wdy, DD Mon YYYY HH:MM:SS GMT"
[[nodiscard]] std::string http_date(std::optional<double> epoch_seconds);

// Base36
[[nodiscard]] std::optional<std::uint64_t> base36_to_int(std::string_view s);
[[nodiscard]] std::string int_to_base36(std::uint64_t i);

// ETags
[[nodiscard]] std::vector<std::string> parse_etags(std::string_view etag_str);
[[nodiscard]] std::string quote_etag(std::string_view etag_str);

// Host / CSRF helpers
[[nodiscard]] bool is_same_domain(std::string_view host, std::string_view pattern);
[[nodiscard]] std::pair<std::string, std::string> split_domain_port(
    std::string_view host);
[[nodiscard]] bool validate_host(std::string_view host,
                                 const std::vector<std::string>& allowed_hosts);

// Content-Disposition header builder (filename ASCII / UTF-8 cases).
// as_attachment + optional filename → header value, or nullopt if no filename
// and not attachment... mirrors Python: returns None when filename is empty
// and not attachment for inline with no name? Actually content_disposition_header
// returns None only if... read Python again.
[[nodiscard]] std::optional<std::string> content_disposition_header(
    bool as_attachment, std::optional<std::string_view> filename);

// urlsafe base64
[[nodiscard]] std::string urlsafe_base64_encode(std::string_view data);
[[nodiscard]] std::optional<std::string> urlsafe_base64_decode(std::string_view s);

}  // namespace django::native
