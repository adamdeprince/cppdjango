#include "scaffold.hpp"

#include <string>

#ifndef DJANGO_NATIVE_VERSION
#define DJANGO_NATIVE_VERSION "0.0.0"
#endif

namespace django::native {

int add(int a, int b) noexcept { return a + b; }

std::string_view version() noexcept {
  return std::string_view{DJANGO_NATIVE_VERSION};
}

std::string_view cxx_standard() noexcept {
  // Under -std=c++26, __cplusplus is typically 202400L today.
  if constexpr (__cplusplus >= 202400L) {
    return "c++26";
  } else if constexpr (__cplusplus >= 202302L) {
    return "c++23";
  } else {
    return "c++20-or-older";
  }
}

std::string compiler() {
#if defined(__GNUC__) && !defined(__clang__)
  return "g++ " + std::to_string(__GNUC__) + "." +
         std::to_string(__GNUC_MINOR__) + "." +
         std::to_string(__GNUC_PATCHLEVEL__);
#elif defined(__clang__)
  return "clang++ " + std::to_string(__clang_major__) + "." +
         std::to_string(__clang_minor__) + "." +
         std::to_string(__clang_patchlevel__);
#else
  return "unknown";
#endif
}

}  // namespace django::native
