#include "wsgi_handler.hpp"

#include <Python.h>

namespace django::native {
namespace {

bool truthy(nb::handle h) { return PyObject_IsTrue(h.ptr()) == 1; }

}  // namespace

bool wsgi_handler_lean_eligible(nb::handle handler) {
  try {
    nb::object empty = handler.attr("_middleware_hooks_empty");
    if (!truthy(empty)) {
      return false;
    }
    nb::object atomic_fn = handler.attr("_any_atomic_requests");
    if (truthy(atomic_fn())) {
      return false;
    }
    return true;
  } catch (...) {
    return false;
  }
}

nb::object wsgi_handler_call(nb::handle handler, nb::handle environ,
                             nb::handle start_response) {
  // Use Python helpers for script-prefix / signals / request class so we stay
  // byte-compatible with stock WSGIHandler while owning the control flow here.

  // set_script_prefix(get_script_name(environ))
  nb::object wsgi_mod = nb::module_::import_("django.core.handlers.wsgi");
  nb::object get_script_name = wsgi_mod.attr("get_script_name");
  nb::object script_name = get_script_name(environ);
  nb::module_::import_("django.urls")
      .attr("set_script_prefix")(script_name);

  // request_started.send(sender=type(handler), environ=environ)
  nb::object handler_type = handler.attr("__class__");
  {
    nb::object send =
        nb::module_::import_("django.core.signals").attr("request_started").attr("send");
    // sender positional; environ keyword via dict call
    nb::dict kwargs;
    kwargs["environ"] = environ;
    nb::tuple args = nb::make_tuple(handler_type);
    nb::steal<nb::object>(
        PyObject_Call(send.ptr(), args.ptr(), kwargs.ptr()));
  }

  // request = handler.request_class(environ)
  nb::object request = handler.attr("request_class")(environ);

  // response = handler.get_response(request)  — views stay Python
  nb::object response = handler.attr("get_response")(request);
  response.attr("_handler_class") = handler_type;

  // status line
  nb::object status = response.attr("wsgi_status_line")();

  // headers: list(response.items()) + Set-Cookie
  nb::list response_headers;
  for (nb::handle item : response.attr("items")()) {
    response_headers.append(item);
  }
  nb::object cookies = response.attr("cookies");
  if (PyObject_IsTrue(cookies.ptr()) == 1 || nb::len(cookies) > 0) {
    for (nb::handle c : cookies.attr("values")()) {
      response_headers.append(
          nb::make_tuple(nb::str("Set-Cookie"), c.attr("OutputString")()));
    }
  }

  // start_response(status, response_headers)
  start_response(status, response_headers);

  // Optional file_wrapper (FileResponse)
  if (nb::hasattr(response, "file_to_stream")) {
    nb::object file_to_stream = response.attr("file_to_stream");
    if (!file_to_stream.is_none()) {
      nb::object file_wrapper =
          environ.attr("get")(nb::str("wsgi.file_wrapper"));
      if (!file_wrapper.is_none()) {
        file_to_stream.attr("close") = response.attr("close");
        return file_wrapper(file_to_stream, response.attr("block_size"));
      }
    }
  }

  return response;
}

}  // namespace django::native
