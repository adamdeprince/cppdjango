// Scaffold primitives for the native acceleration layer.
#pragma once

#include <string>
#include <string_view>

namespace django::native {

[[nodiscard]] int add(int a, int b) noexcept;

[[nodiscard]] std::string_view version() noexcept;

[[nodiscard]] std::string_view cxx_standard() noexcept;

[[nodiscard]] std::string compiler();

}  // namespace django::native
