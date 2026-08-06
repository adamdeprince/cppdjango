#include "wsgi_handler.hpp"

#include "orm.hpp"  // wsgi_request_path

#include <Python.h>

#include <optional>
#include <string>
#include <string_view>

namespace django::native {
namespace {

bool truthy(nb::handle h) { return PyObject_IsTrue(h.ptr()) == 1; }

std::string upper_ascii(std::string s) {
  for (char& c : s) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  return s;
}

bool is_ascii(std::string_view s) {
  for (unsigned char c : s) {
    if (c > 127) {
      return false;
    }
  }
  return true;
}

std::string environ_str(nb::dict env, const char* key, const char* deflt) {
  if (!env.contains(key)) {
    return deflt;
  }
  nb::handle v = env[key];
  if (v.is_none()) {
    return deflt;
  }
  return nb::cast<std::string>(nb::str(v));
}

// SCRIPT_NAME for the common case: no FORCE_SCRIPT_NAME, no mod_rewrite URL.
std::optional<std::string> script_name_fast(nb::dict env) {
  try {
    nb::object settings =
        nb::module_::import_("django.conf").attr("settings");
    if (!settings.attr("FORCE_SCRIPT_NAME").is_none()) {
      return std::nullopt;
    }
  } catch (...) {
    return std::nullopt;
  }
  std::string script_url = environ_str(env, "SCRIPT_URL", "");
  if (script_url.empty()) {
    script_url = environ_str(env, "REDIRECT_URL", "");
  }
  if (!script_url.empty()) {
    return std::nullopt;
  }
  std::string sn = environ_str(env, "SCRIPT_NAME", "");
  if (!is_ascii(sn)) {
    return std::nullopt;
  }
  return sn;
}

std::optional<std::string> path_info_fast(nb::dict env) {
  std::string pi = environ_str(env, "PATH_INFO", "/");
  if (pi.empty()) {
    return std::string("/");
  }
  if (!is_ascii(pi)) {
    return std::nullopt;
  }
  return pi;
}

nb::object empty_stream() {
  return nb::module_::import_("io").attr("BytesIO")(nb::bytes(""));
}

void append_set_cookies(nb::list& response_headers, nb::handle response) {
  nb::object cookies = response.attr("cookies");
  if (PyObject_IsTrue(cookies.ptr()) != 1 && nb::len(cookies) == 0) {
    return;
  }
  for (nb::handle c : cookies.attr("values")()) {
    response_headers.append(
        nb::make_tuple(nb::str("Set-Cookie"), c.attr("OutputString")()));
  }
}

// callback(request, *args, **kwargs)
nb::object call_view(nb::handle callback, nb::handle request, nb::handle args,
                     nb::handle kwargs) {
  nb::list pos;
  pos.append(request);
  if (!args.is_none()) {
    for (nb::handle a : args) {
      pos.append(a);
    }
  }
  nb::tuple call_args = nb::tuple(pos);
  PyObject* kw_ptr = nullptr;
  nb::dict kw_holder;
  if (!kwargs.is_none()) {
    if (PyDict_Check(kwargs.ptr())) {
      kw_ptr = kwargs.ptr();
    } else {
      // Copy mapping into a real dict.
      for (nb::handle item : kwargs.attr("items")()) {
        nb::tuple t = nb::cast<nb::tuple>(item);
        kw_holder[t[0]] = t[1];
      }
      kw_ptr = kw_holder.ptr();
    }
  } else {
    kw_ptr = kw_holder.ptr();  // empty dict
  }
  PyObject* result =
      PyObject_Call(callback.ptr(), call_args.ptr(), kw_ptr);
  if (!result) {
    throw nb::python_error();
  }
  return nb::steal<nb::object>(result);
}

nb::object response_for_python_error(nb::handle request, nb::python_error& e) {
  e.restore();
  PyObject *ptype = nullptr, *pvalue = nullptr, *ptrace = nullptr;
  PyErr_Fetch(&ptype, &pvalue, &ptrace);
  PyErr_NormalizeException(&ptype, &pvalue, &ptrace);
  if (pvalue == nullptr) {
    Py_XDECREF(ptype);
    Py_XDECREF(ptrace);
    throw nb::python_error();
  }
  nb::object exc = nb::steal<nb::object>(pvalue);
  Py_XDECREF(ptype);
  Py_XDECREF(ptrace);
  nb::object rfe = nb::module_::import_("django.core.handlers.exception")
                       .attr("response_for_exception");
  return rfe(request, exc);
}

}  // namespace

bool wsgi_handler_lean_eligible(nb::handle handler) {
  try {
    if (!truthy(handler.attr("_middleware_hooks_empty"))) {
      return false;
    }
    if (truthy(handler.attr("_any_atomic_requests")())) {
      return false;
    }
    return true;
  } catch (...) {
    return false;
  }
}

bool wsgi_environ_is_lean_get(nb::handle environ_h) noexcept {
  try {
    nb::dict env = nb::cast<nb::dict>(environ_h);
    std::string method = upper_ascii(environ_str(env, "REQUEST_METHOD", ""));
    if (method != "GET" && method != "HEAD") {
      return false;
    }
    if (env.contains("HTTP_TRANSFER_ENCODING")) {
      if (!environ_str(env, "HTTP_TRANSFER_ENCODING", "").empty()) {
        return false;
      }
    }
    std::string cl = environ_str(env, "CONTENT_LENGTH", "");
    if (!cl.empty() && cl != "0") {
      try {
        if (std::stoll(cl) != 0) {
          return false;
        }
      } catch (...) {
        return false;
      }
    }
    return true;
  } catch (...) {
    return false;
  }
}

bool wsgi_request_try_lean_init(nb::handle request, nb::handle environ_h) {
  if (!wsgi_environ_is_lean_get(environ_h)) {
    return false;
  }
  nb::dict env = nb::cast<nb::dict>(environ_h);

  auto sn_opt = script_name_fast(env);
  auto pi_opt = path_info_fast(env);
  if (!sn_opt || !pi_opt) {
    return false;
  }
  std::string script_name = *sn_opt;
  std::string path_info = *pi_opt;
  if (path_info.empty()) {
    path_info = "/";
  }

  std::string method =
      upper_ascii(environ_str(env, "REQUEST_METHOD", "GET"));
  std::string path = wsgi_request_path(script_name, path_info);

  env["PATH_INFO"] = nb::str(path_info.c_str(), path_info.size());
  env["SCRIPT_NAME"] = nb::str(script_name.c_str(), script_name.size());

  request.attr("environ") = env;
  request.attr("path_info") = nb::str(path_info.c_str(), path_info.size());
  request.attr("path") = nb::str(path.c_str(), path.size());
  request.attr("META") = env;
  request.attr("method") = nb::str(method.c_str(), method.size());
  request.attr("content_type") = nb::str("");
  request.attr("content_params") = nb::dict();
  request.attr("_stream") = empty_stream();
  request.attr("_read_started") = false;
  request.attr("resolver_match") = nb::none();
  return true;
}

nb::object wsgi_lean_get_response(nb::handle handler, nb::handle request) {
  handler.attr("_ensure_root_urlconf")();

  nb::object callback;
  nb::object args;
  nb::object kwargs;
  bool resolved = false;

  if (nb::hasattr(handler, "_exact_routes")) {
    nb::object exact = handler.attr("_exact_routes");
    if (!exact.is_none() && truthy(exact)) {
      nb::object path_info = request.attr("path_info");
      nb::dict table = nb::cast<nb::dict>(exact);
      if (table.contains(path_info)) {
        nb::tuple t = nb::cast<nb::tuple>(table[path_info]);
        callback = nb::borrow(nb::object(t[0]));
        args = nb::borrow(nb::object(t[1]));
        kwargs = nb::borrow(nb::object(t[2]));
        nb::object url_name = nb::len(t) >= 4 ? nb::borrow(nb::object(t[3]))
                                              : nb::none();
        nb::object route =
            nb::len(t) >= 5 ? nb::borrow(nb::object(t[4])) : nb::none();
        nb::object RM =
            nb::module_::import_("django.urls.resolvers").attr("ResolverMatch");
        // func, args, kwargs, url_name=None, app_names=None, namespaces=None, route=None
        nb::object match =
            RM(callback, args, kwargs, url_name, nb::none(), nb::none(), route);
        request.attr("resolver_match") = match;
        resolved = true;
      }
    }
  }

  if (!resolved) {
    nb::object match = handler.attr("resolve_request")(request);
    callback = match.attr("func");
    args = match.attr("args");
    kwargs = match.attr("kwargs");
  }

  nb::object response = call_view(callback, request, args, kwargs);
  handler.attr("check_response")(response, callback);

  if (nb::hasattr(response, "render")) {
    nb::object render = response.attr("render");
    if (truthy(nb::module_::import_("builtins").attr("callable")(render))) {
      response = render();
    }
  }

  response.attr("_resource_closers").attr("append")(request.attr("close"));

  if (nb::cast<int>(response.attr("status_code")) >= 400) {
    nb::object log_response =
        nb::module_::import_("django.utils.log").attr("log_response");
    nb::dict kw;
    kw["response"] = response;
    kw["request"] = request;
    nb::tuple log_args = nb::make_tuple(nb::str("%s: %s"),
                                        response.attr("reason_phrase"),
                                        request.attr("path"));
    nb::steal<nb::object>(
        PyObject_Call(log_response.ptr(), log_args.ptr(), kw.ptr()));
  }
  return response;
}

nb::object wsgi_handler_call(nb::handle handler, nb::handle environ,
                             nb::handle start_response) {
  nb::object wsgi_mod = nb::module_::import_("django.core.handlers.wsgi");
  nb::dict env = nb::cast<nb::dict>(environ);

  // script prefix
  nb::object script_name;
  if (auto sn = script_name_fast(env)) {
    script_name = nb::str(sn->c_str(), sn->size());
  } else {
    script_name = wsgi_mod.attr("get_script_name")(environ);
  }
  nb::module_::import_("django.urls").attr("set_script_prefix")(script_name);

  // request_started
  nb::object handler_type = handler.attr("__class__");
  {
    nb::object send = nb::module_::import_("django.core.signals")
                          .attr("request_started")
                          .attr("send");
    nb::dict kwargs;
    kwargs["environ"] = environ;
    nb::tuple args = nb::make_tuple(handler_type);
    nb::steal<nb::object>(
        PyObject_Call(send.ptr(), args.ptr(), kwargs.ptr()));
  }

  // request — WSGIRequest.__init__ dual-paths into try_lean_init
  nb::object request = handler.attr("request_class")(environ);

  // lean get_response with exception conversion
  nb::object response;
  try {
    response = wsgi_lean_get_response(handler, request);
  } catch (nb::python_error& e) {
    response = response_for_python_error(request, e);
  }

  response.attr("_handler_class") = handler_type;

  nb::object status = response.attr("wsgi_status_line")();
  nb::list response_headers;
  for (nb::handle item : response.attr("items")()) {
    response_headers.append(item);
  }
  append_set_cookies(response_headers, response);
  start_response(status, response_headers);

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
