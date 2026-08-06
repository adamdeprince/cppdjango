# Django native acceleration layer (nanobind + C++26)
cmake_minimum_required(VERSION 3.26...4.3)

project(
  ${SKBUILD_PROJECT_NAME}
  VERSION ${SKBUILD_PROJECT_VERSION}
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(
  Python
  REQUIRED
  COMPONENTS Interpreter Development.Module
  OPTIONAL_COMPONENTS Development.SABIModule
)

# Resolve nanobind from the active interpreter first so editable rebuilds
# work outside pip's isolated build environment.
if(NOT nanobind_DIR AND NOT nanobind_ROOT)
  execute_process(
    COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    OUTPUT_VARIABLE _django_nanobind_cmake_dir
    RESULT_VARIABLE _django_nanobind_cmake_status
    ERROR_QUIET
  )
  if(_django_nanobind_cmake_status EQUAL 0 AND _django_nanobind_cmake_dir)
    set(nanobind_DIR "${_django_nanobind_cmake_dir}" CACHE PATH "nanobind CMake dir")
  endif()
endif()

find_package(nanobind CONFIG REQUIRED)

set(DJANGO_NATIVE_SOURCES
  cpp/scaffold.cpp
  cpp/html.cpp
  cpp/text.cpp
  cpp/dateparse.cpp
  cpp/http.cpp
  cpp/urls.cpp
  cpp/template.cpp
  cpp/template_expr.cpp
  cpp/filters.cpp
  cpp/cookie.cpp
  cpp/request_utils.cpp
  cpp/multipart.cpp
  cpp/locale.cpp
  cpp/validators.cpp
  cpp/signing.cpp
  cpp/crypto.cpp
  cpp/urlize.cpp
  cpp/cache.cpp
  cpp/datastructures.cpp
  cpp/orm.cpp
  cpp/orm_engine/schema.cpp
  cpp/orm_engine/query.cpp
  cpp/orm_engine/compile.cpp
  cpp/orm_engine/queryset.cpp
  cpp/orm_engine/bind.cpp
  cpp/wsgi_handler.cpp
  cpp/middleware.cpp
  cpp/module.cpp
)

nanobind_add_module(
  _native
  NB_STATIC
  ${DJANGO_NATIVE_SOURCES}
)

target_include_directories(_native PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cpp)

# OpenSSL for HMAC / PBKDF2 / RAND (system crypto, not homebrew invent)
find_package(OpenSSL REQUIRED)
target_link_libraries(_native PRIVATE OpenSSL::Crypto)

target_compile_definitions(
  _native
  PRIVATE
    DJANGO_NATIVE_VERSION="${PROJECT_VERSION}"
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(
    _native
    PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -O3
  )
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(
    _native
    PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -O3
  )
endif()

install(TARGETS _native LIBRARY DESTINATION django)
