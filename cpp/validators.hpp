// Form / core validation helpers (django.core.validators + forms.fields).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace django::native {

// slug_re: ^[-a-zA-Z0-9_]+\Z
[[nodiscard]] bool is_valid_slug(std::string_view value) noexcept;

// integer_validator: ^-?\d+\Z
[[nodiscard]] bool is_valid_integer_string(std::string_view value) noexcept;

// IntegerField.to_python core: strip trailing \.0*\s* then parse int.
// nullopt → invalid (caller raises ValidationError).
[[nodiscard]] std::optional<std::int64_t> form_integer_to_python(
    std::string_view value);

// IPv4 dotted-quad (ipaddress.IPv4Address semantics for decimal octets 0-255).
[[nodiscard]] bool is_valid_ipv4(std::string_view value) noexcept;

// IPv6 (including compressed and IPv4-mapped). Structural check aligned with
// common ipaddress acceptance; exotic edge cases may fall back in Python.
[[nodiscard]] bool is_valid_ipv6(std::string_view value) noexcept;

// IPv4 or IPv6.
[[nodiscard]] bool is_valid_ipv46(std::string_view value) noexcept;

// Email: length ≤ 320, one @, user + domain structure.
// domain_allowlist entries are exact domain matches (e.g. "localhost").
// Returns true if valid. Does not raise — Python wraps ValidationError.
[[nodiscard]] bool is_valid_email(std::string_view value,
                                  const std::vector<std::string>& domain_allowlist);

// ProhibitNullCharactersValidator
[[nodiscard]] bool has_null_characters(std::string_view value) noexcept;

// CharField strip path: str then optional strip; empty → empty_value.
// empty_values checked in Python; this only transforms non-empty strings.
[[nodiscard]] std::string char_field_strip(std::string_view value, bool strip);

// DomainNameValidator-style check (ASCII + optional IDNA labels).
// max_length default 255. Returns false for structural rejects; Python regex
// remains oracle for exotic Unicode edge cases when this returns true.
[[nodiscard]] bool is_valid_domain_name(std::string_view value, bool accept_idna,
                                        int max_length = 255) noexcept;

// URLValidator structural precheck beyond length/unsafe whitespace:
// requires scheme://, scheme in schemes list (comma-separated lowercase ASCII),
// no unsafe whitespace, length ≤ max_length.
[[nodiscard]] bool url_structure_precheck(std::string_view value, int max_length,
                                          std::string_view schemes_csv) noexcept;

}  // namespace django::native
