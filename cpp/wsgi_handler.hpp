// Native WSGI request loop (dual-path).
//
// Architecture: the lean WSGI *handler* lives in C++. Views remain Python.
// Lean path (empty middleware, no ATOMIC_REQUESTS):
//   environ → slim request (GET/HEAD empty body) → exact-route or resolve
//   → Python view → start_response pack → body
//
// Complex middleware stacks fall back to pure-Python WSGIHandler.
#pragma once

#include <nanobind/nanobind.h>

namespace nb = nanobind;

namespace django::native {

// True when the handler is eligible for the native lean loop (empty middleware
// hooks and no ATOMIC_REQUESTS).
[[nodiscard]] bool wsgi_handler_lean_eligible(nb::handle handler);

// Run one lean WSGI request. Owns: script prefix, signals, slim request,
// lean get_response (exact routes / resolve + view), start_response packing.
// Returns the WSGI body iterable (typically the HttpResponse itself).
[[nodiscard]] nb::object wsgi_handler_call(nb::handle handler, nb::handle environ,
                                           nb::handle start_response);

// Populate a WSGIRequest instance for GET/HEAD with empty body.
// Returns true if lean init applied; false → caller must use full Python init.
[[nodiscard]] bool wsgi_request_try_lean_init(nb::handle request,
                                              nb::handle environ);

// Lean get_response: urlconf pin, exact-route table or resolve_request, call
// view, attach request.close. Raises into Python on errors (caller wraps).
[[nodiscard]] nb::object wsgi_lean_get_response(nb::handle handler,
                                                nb::handle request);

// True when environ is GET/HEAD with no body (lean request eligible).
[[nodiscard]] bool wsgi_environ_is_lean_get(nb::handle environ) noexcept;

// Pack HttpResponse into start_response(status, headers) using C API
// (_store walk, cached "200 OK", skip empty cookies). Returns response
// (or file_wrapper result). Used by the lean WSGI outer loop.
[[nodiscard]] nb::object wsgi_pack_start_response(nb::handle response,
                                                  nb::handle start_response,
                                                  nb::handle environ);

}  // namespace django::native
