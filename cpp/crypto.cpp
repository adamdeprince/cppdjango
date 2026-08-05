#include "crypto.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <stdexcept>
#include <string>
#include <string_view>

namespace django::native {
namespace {

const EVP_MD* evp_md(HashAlgo algo) {
  switch (algo) {
    case HashAlgo::Sha1:
      return EVP_sha1();
    case HashAlgo::Sha256:
      return EVP_sha256();
    case HashAlgo::Sha384:
      return EVP_sha384();
    case HashAlgo::Sha512:
      return EVP_sha512();
    case HashAlgo::Md5:
      return EVP_md5();
  }
  return EVP_sha256();
}

}  // namespace

std::optional<HashAlgo> hash_algo_from_name(std::string_view name) {
  if (name == "sha1") {
    return HashAlgo::Sha1;
  }
  if (name == "sha256") {
    return HashAlgo::Sha256;
  }
  if (name == "sha384") {
    return HashAlgo::Sha384;
  }
  if (name == "sha512") {
    return HashAlgo::Sha512;
  }
  if (name == "md5") {
    return HashAlgo::Md5;
  }
  return std::nullopt;
}

std::string hash_digest(HashAlgo algo, std::string_view data) {
  const EVP_MD* md = evp_md(algo);
  unsigned char out[EVP_MAX_MD_SIZE];
  unsigned int out_len = 0;
  if (EVP_Digest(data.data(), data.size(), out, &out_len, md, nullptr) != 1) {
    throw std::runtime_error("EVP_Digest failed");
  }
  return std::string(reinterpret_cast<char*>(out), out_len);
}

std::string hmac_digest(HashAlgo algo, std::string_view key, std::string_view msg) {
  const EVP_MD* md = evp_md(algo);
  unsigned char out[EVP_MAX_MD_SIZE];
  unsigned int out_len = 0;
  if (HMAC(md, key.data(), static_cast<int>(key.size()),
            reinterpret_cast<const unsigned char*>(msg.data()), msg.size(), out,
            &out_len) == nullptr) {
    throw std::runtime_error("HMAC failed");
  }
  return std::string(reinterpret_cast<char*>(out), out_len);
}

std::string salted_hmac_digest(HashAlgo algo, std::string_view key_salt,
                               std::string_view secret, std::string_view value) {
  // key = H(key_salt + secret)
  std::string material;
  material.reserve(key_salt.size() + secret.size());
  material.append(key_salt);
  material.append(secret);
  const std::string key = hash_digest(algo, material);
  return hmac_digest(algo, key, value);
}

std::string pbkdf2_hmac(HashAlgo algo, std::string_view password, std::string_view salt,
                        int iterations, int dklen) {
  const EVP_MD* md = evp_md(algo);
  const int md_size = EVP_MD_size(md);
  if (dklen <= 0) {
    dklen = md_size;
  }
  if (iterations < 1 || dklen < 1) {
    throw std::invalid_argument("invalid pbkdf2 params");
  }
  std::string out(static_cast<std::size_t>(dklen), '\0');
  if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                        reinterpret_cast<const unsigned char*>(salt.data()),
                        static_cast<int>(salt.size()), iterations, md, dklen,
                        reinterpret_cast<unsigned char*>(out.data())) != 1) {
    throw std::runtime_error("PBKDF2 failed");
  }
  return out;
}

std::string secure_random_string(int length, std::string_view allowed_chars) {
  if (length < 0) {
    throw std::invalid_argument("negative length");
  }
  if (allowed_chars.empty()) {
    throw std::invalid_argument("empty alphabet");
  }
  if (length == 0) {
    return {};
  }
  const std::size_t n = allowed_chars.size();
  // Rejection sampling to avoid modulo bias when n doesn't divide 256.
  const unsigned limit = 256 - (256 % static_cast<unsigned>(n));
  std::string out;
  out.reserve(static_cast<std::size_t>(length));
  while (static_cast<int>(out.size()) < length) {
    unsigned char byte = 0;
    if (RAND_bytes(&byte, 1) != 1) {
      throw std::runtime_error("RAND_bytes failed");
    }
    if (byte >= limit) {
      continue;
    }
    out += allowed_chars[byte % n];
  }
  return out;
}

}  // namespace django::native
