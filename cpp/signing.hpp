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

}  // namespace django::native
