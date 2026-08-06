// Native WSGI request loop (dual-path).
//
// Architecture north star: the WSGI *handler* lives in C++. Views remain
// Python (Django's public API contract). ORM / templates / forms / auth are
// separate planes that may also move to C++ over time; this module only owns
// the per-request WSGI orchestration:
//
//   environ → request → resolve → view (Python) → start_response → body
//
// Complex middleware stacks fall back to the pure-Python WSGIHandler.
#pragma once

#include <nanobind/nanobind.h>

namespace nb = nanobind;

namespace django::native {

// Run one WSGI request on an initialized Django WSGIHandler instance.
//
// handler: Python WSGIHandler (or subclass) with load_middleware done
// environ: WSGI environ dict
// start_response: WSGI start_response callable
//
// Returns the WSGI body iterable (typically the HttpResponse itself).
[[nodiscard]] nb::object wsgi_handler_call(nb::handle handler, nb::handle environ,
                                           nb::handle start_response);

// True when the handler is eligible for the native lean loop (empty middleware
// hooks and no ATOMIC_REQUESTS). Python may also check this before calling.
[[nodiscard]] bool wsgi_handler_lean_eligible(nb::handle handler);

}  // namespace django::native
