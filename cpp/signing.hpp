// django.core.signing mechanical helpers (base62, urlsafe b64).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace django::native {

// BASE62_ALPHABET encode/decode (signed integers as in Django).
[[nodiscard]] std::string b62_encode(std::int64_t value);
[[nodiscard]] std::optional<std::int64_t> b62_decode(std::string_view s);

// urlsafe base64 without padding (signing.b64_encode / b64_decode).
[[nodiscard]] std::string b64_encode(std::string_view data);
[[nodiscard]] std::optional<std::string> b64_decode(std::string_view data);

// Constant-time equality for signature bytes/strings.
[[nodiscard]] bool constant_time_compare(std::string_view a, std::string_view b) noexcept;

// Sep safety: True if sep is empty or only [A-Za-z0-9-_=] (unsafe for Signer).
[[nodiscard]] bool signer_sep_unsafe(std::string_view sep) noexcept;

// Django signing.base64_hmac: urlsafe b64 (no pad) of salted_hmac digest.
// algorithm: "sha1" | "sha256" | "sha384" | "sha512" | "md5".
[[nodiscard]] std::optional<std::string> signing_base64_hmac(
    std::string_view salt, std::string_view value, std::string_view key,
    std::string_view algorithm);

// TimestampSigner.unsign_object core (max_age skipped when max_age_seconds < 0).
// Returns serializer payload bytes (after b64 decode + optional zlib).
// nullopt on bad signature / corrupt payload (caller tries fallback keys).
[[nodiscard]] std::optional<std::string> signing_unsign_object_bytes(
    std::string_view signed_value, std::string_view salt,
    std::string_view secret, std::string_view algorithm = "sha256",
    std::string_view sep = ":", double max_age_seconds = -1.0,
    double now_unix = 0.0);

}  // namespace django::native
