// System-crypto helpers (OpenSSL): HMAC, PBKDF2, CSPRNG.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace django::native {

enum class HashAlgo {
  Sha1,
  Sha256,
  Sha384,
  Sha512,
  Md5,
};

[[nodiscard]] std::optional<HashAlgo> hash_algo_from_name(std::string_view name);

// hashlib-style digest of data.
[[nodiscard]] std::string hash_digest(HashAlgo algo, std::string_view data);

// HMAC(key, msg) raw digest.
[[nodiscard]] std::string hmac_digest(HashAlgo algo, std::string_view key,
                                      std::string_view msg);

// Django salted_hmac: key = H(key_salt + secret); return HMAC(key, value).
[[nodiscard]] std::string salted_hmac_digest(HashAlgo algo, std::string_view key_salt,
                                             std::string_view secret,
                                             std::string_view value);

// PBKDF2-HMAC; dklen 0 means hash length.
[[nodiscard]] std::string pbkdf2_hmac(HashAlgo algo, std::string_view password,
                                      std::string_view salt, int iterations, int dklen);

// CSPRNG string from allowed_chars.
[[nodiscard]] std::string secure_random_string(int length, std::string_view allowed_chars);

}  // namespace django::native
