#include "wsgi_handler.hpp"

#include "middleware.hpp"  // native_stock_chain_call
#include "orm.hpp"         // wsgi_request_path

#include <Python.h>

#include <optional>
#include <string>
#include <string_view>

namespace django::native {
namespace {

// ---------------------------------------------------------------------------
// Process-lifetime cache of interned names + module callables (item 1).
// Populated once under the GIL; never cleared (Django process model).
// ---------------------------------------------------------------------------
struct LeanCache {
  bool ready = false;

  // Interned attribute / dict key names
  PyObject* name_environ = nullptr;
  PyObject* name_path_info = nullptr;
  PyObject* name_path = nullptr;
  PyObject* name_META = nullptr;
  PyObject* name_method = nullptr;
  PyObject* name_content_type = nullptr;
  PyObject* name_content_params = nullptr;
  PyObject* name__stream = nullptr;
  PyObject* name__read_started = nullptr;
  PyObject* name_resolver_match = nullptr;
  PyObject* name_close = nullptr;
  PyObject* name__resource_closers = nullptr;
  PyObject* name_append = nullptr;
  PyObject* name_status_code = nullptr;
  PyObject* name_cookies = nullptr;
  PyObject* name_items = nullptr;
  PyObject* name_headers = nullptr;
  PyObject* name__store = nullptr;
  PyObject* name__reason_phrase = nullptr;
  PyObject* name_wsgi_status_line = nullptr;
  PyObject* name__handler_class = nullptr;
  PyObject* name__exact_routes = nullptr;
  PyObject* name__exact_callbacks = nullptr;
  PyObject* name_request_class = nullptr;
  PyObject* name__middleware_hooks_empty = nullptr;
  PyObject* name__any_atomic_requests = nullptr;
  PyObject* name__lean_urlconf_pinned = nullptr;
  PyObject* name_file_to_stream = nullptr;
  PyObject* name_block_size = nullptr;
  PyObject* name_func = nullptr;
  PyObject* name_args = nullptr;
  PyObject* name_kwargs = nullptr;
  PyObject* name_values = nullptr;
  PyObject* name_OutputString = nullptr;
  PyObject* name_render = nullptr;

  // Constant strings
  PyObject* str_empty = nullptr;
  PyObject* str_200_ok = nullptr;
  PyObject* str_set_cookie = nullptr;
  PyObject* str_PATH_INFO = nullptr;
  PyObject* str_SCRIPT_NAME = nullptr;
  PyObject* str_REQUEST_METHOD = nullptr;
  PyObject* str_CONTENT_LENGTH = nullptr;
  PyObject* str_SCRIPT_URL = nullptr;
  PyObject* str_REDIRECT_URL = nullptr;
  PyObject* str_HTTP_TRANSFER_ENCODING = nullptr;
  PyObject* str_GET = nullptr;
  PyObject* str_HEAD = nullptr;
  PyObject* str_wsgi_file_wrapper = nullptr;
  PyObject* bytes_empty = nullptr;

  // Callables / objects (borrowed from modules; held with INCREF)
  PyObject* set_script_prefix = nullptr;
  PyObject* request_started = nullptr;       // Signal instance
  PyObject* request_started_send = nullptr;  // unbound only if needed
  PyObject* get_script_name = nullptr;
  PyObject* BytesIO = nullptr;
  PyObject* response_for_exception = nullptr;
  PyObject* empty_tuple = nullptr;
  PyObject* empty_dict = nullptr;

};

LeanCache g_lean;

void lean_cache_init() {
  if (g_lean.ready) {
    return;
  }
  auto intern = [](const char* s) {
    PyObject* o = PyUnicode_InternFromString(s);
    return o;
  };
  g_lean.name_environ = intern("environ");
  g_lean.name_path_info = intern("path_info");
  g_lean.name_path = intern("path");
  g_lean.name_META = intern("META");
  g_lean.name_method = intern("method");
  g_lean.name_content_type = intern("content_type");
  g_lean.name_content_params = intern("content_params");
  g_lean.name__stream = intern("_stream");
  g_lean.name__read_started = intern("_read_started");
  g_lean.name_resolver_match = intern("resolver_match");
  g_lean.name_close = intern("close");
  g_lean.name__resource_closers = intern("_resource_closers");
  g_lean.name_append = intern("append");
  g_lean.name_status_code = intern("status_code");
  g_lean.name_cookies = intern("cookies");
  g_lean.name_items = intern("items");
  g_lean.name_headers = intern("headers");
  g_lean.name__store = intern("_store");
  g_lean.name__reason_phrase = intern("_reason_phrase");
  g_lean.name_wsgi_status_line = intern("wsgi_status_line");
  g_lean.name__handler_class = intern("_handler_class");
  g_lean.name__exact_routes = intern("_exact_routes");
  g_lean.name__exact_callbacks = intern("_exact_callbacks");
  g_lean.name_request_class = intern("request_class");
  g_lean.name__middleware_hooks_empty = intern("_middleware_hooks_empty");
  g_lean.name__any_atomic_requests = intern("_any_atomic_requests");
  g_lean.name__lean_urlconf_pinned = intern("_lean_urlconf_pinned");
  g_lean.name_file_to_stream = intern("file_to_stream");
  g_lean.name_block_size = intern("block_size");
  g_lean.name_func = intern("func");
  g_lean.name_args = intern("args");
  g_lean.name_kwargs = intern("kwargs");
  g_lean.name_values = intern("values");
  g_lean.name_OutputString = intern("OutputString");
  g_lean.name_render = intern("render");

  g_lean.str_empty = intern("");
  g_lean.str_200_ok = intern("200 OK");
  g_lean.str_set_cookie = intern("Set-Cookie");
  g_lean.str_PATH_INFO = intern("PATH_INFO");
  g_lean.str_SCRIPT_NAME = intern("SCRIPT_NAME");
  g_lean.str_REQUEST_METHOD = intern("REQUEST_METHOD");
  g_lean.str_CONTENT_LENGTH = intern("CONTENT_LENGTH");
  g_lean.str_SCRIPT_URL = intern("SCRIPT_URL");
  g_lean.str_REDIRECT_URL = intern("REDIRECT_URL");
  g_lean.str_HTTP_TRANSFER_ENCODING = intern("HTTP_TRANSFER_ENCODING");
  g_lean.str_GET = intern("GET");
  g_lean.str_HEAD = intern("HEAD");
  g_lean.str_wsgi_file_wrapper = intern("wsgi.file_wrapper");
  g_lean.bytes_empty = PyBytes_FromStringAndSize("", 0);

  nb::object urls = nb::module_::import_("django.urls");
  g_lean.set_script_prefix = urls.attr("set_script_prefix").inc_ref().ptr();

  nb::object signals = nb::module_::import_("django.core.signals");
  g_lean.request_started = signals.attr("request_started").inc_ref().ptr();
  g_lean.request_started_send =
      signals.attr("request_started").attr("send").inc_ref().ptr();

  nb::object wsgi = nb::module_::import_("django.core.handlers.wsgi");
  g_lean.get_script_name = wsgi.attr("get_script_name").inc_ref().ptr();

  nb::object io = nb::module_::import_("io");
  g_lean.BytesIO = io.attr("BytesIO").inc_ref().ptr();

  nb::object exc = nb::module_::import_("django.core.handlers.exception");
  g_lean.response_for_exception =
      exc.attr("response_for_exception").inc_ref().ptr();

  g_lean.empty_tuple = PyTuple_New(0);
  g_lean.empty_dict = PyDict_New();

  g_lean.ready = true;
}

inline bool truthy(PyObject* o) {
  int r = PyObject_IsTrue(o);
  return r == 1;
}

// Stable/limited-API-safe callables (avoid PyObject_CallOneArg / PyCoro_*).
inline PyObject* call_one(PyObject* fn, PyObject* arg) {
  PyObject* t = PyTuple_Pack(1, arg);
  if (!t) {
    return nullptr;
  }
  PyObject* r = PyObject_Call(fn, t, nullptr);
  Py_DECREF(t);
  return r;
}

inline PyObject* call_noargs(PyObject* fn) {
  return PyObject_Call(fn, g_lean.empty_tuple, nullptr);
}

bool is_ascii(std::string_view s) {
  for (unsigned char c : s) {
    if (c > 127) {
      return false;
    }
  }
  return true;
}

std::string upper_ascii(std::string s) {
  for (char& c : s) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  return s;
}

// Borrowed str from environ dict, or nullptr if missing.
PyObject* env_get(PyObject* env, PyObject* key) {
  PyObject* v = PyDict_GetItem(env, key);  // borrowed
  return v;
}

std::string py_to_std(PyObject* o) {
  if (o == nullptr || o == Py_None) {
    return {};
  }
  PyObject* s = PyObject_Str(o);
  if (!s) {
    PyErr_Clear();
    return {};
  }
  Py_ssize_t n = 0;
  const char* p = PyUnicode_AsUTF8AndSize(s, &n);
  std::string out = p ? std::string(p, static_cast<std::size_t>(n)) : std::string();
  Py_DECREF(s);
  return out;
}

std::optional<std::string> script_name_fast(PyObject* env) {
  lean_cache_init();
  // Do not cache FORCE_SCRIPT_NAME — tests (and rare deploys) toggle it.
  try {
    nb::object settings =
        nb::module_::import_("django.conf").attr("settings");
    if (!settings.attr("FORCE_SCRIPT_NAME").is_none()) {
      return std::nullopt;
    }
  } catch (...) {
    return std::nullopt;
  }
  PyObject* script_url = env_get(env, g_lean.str_SCRIPT_URL);
  if (script_url && script_url != Py_None) {
    std::string s = py_to_std(script_url);
    if (!s.empty()) {
      return std::nullopt;
    }
  }
  PyObject* redir = env_get(env, g_lean.str_REDIRECT_URL);
  if (redir && redir != Py_None) {
    std::string s = py_to_std(redir);
    if (!s.empty()) {
      return std::nullopt;
    }
  }
  PyObject* sn_o = env_get(env, g_lean.str_SCRIPT_NAME);
  std::string sn = sn_o ? py_to_std(sn_o) : std::string();
  if (!is_ascii(sn)) {
    return std::nullopt;
  }
  return sn;
}

std::optional<std::string> path_info_fast(PyObject* env) {
  PyObject* pi_o = env_get(env, g_lean.str_PATH_INFO);
  std::string pi = pi_o ? py_to_std(pi_o) : std::string("/");
  if (pi.empty()) {
    return std::string("/");
  }
  if (!is_ascii(pi)) {
    return std::nullopt;
  }
  return pi;
}

// Item 3: skip request_started when Signal.receivers is empty.
bool request_started_has_receivers() {
  lean_cache_init();
  PyObject* sig = g_lean.request_started;
  if (!sig) {
    return true;  // unknown → send
  }
  PyObject* receivers = PyObject_GetAttrString(sig, "receivers");
  if (!receivers) {
    PyErr_Clear();
    return true;
  }
  Py_ssize_t n = PyObject_Size(receivers);
  Py_DECREF(receivers);
  if (n < 0) {
    PyErr_Clear();
    return true;
  }
  return n > 0;
}

void maybe_send_request_started(PyObject* handler_type, PyObject* environ) {
  if (!request_started_has_receivers()) {
    return;
  }
  lean_cache_init();
  PyObject* kwargs = PyDict_New();
  if (!kwargs) {
    throw nb::python_error();
  }
  if (PyDict_SetItemString(kwargs, "environ", environ) < 0) {
    Py_DECREF(kwargs);
    throw nb::python_error();
  }
  PyObject* args = PyTuple_Pack(1, handler_type);
  if (!args) {
    Py_DECREF(kwargs);
    throw nb::python_error();
  }
  PyObject* res =
      PyObject_Call(g_lean.request_started_send, args, kwargs);
  Py_DECREF(args);
  Py_DECREF(kwargs);
  if (!res) {
    throw nb::python_error();
  }
  Py_DECREF(res);
}

// Fast view call: TE routes have empty args/kwargs.
nb::object call_view_fast(PyObject* callback, PyObject* request, PyObject* args,
                          PyObject* kwargs) {
  // Empty args + empty/None kwargs → CallOneArg
  bool args_empty =
      args == nullptr || args == Py_None ||
      (PyTuple_Check(args) && PyTuple_GET_SIZE(args) == 0) ||
      (PyList_Check(args) && PyList_GET_SIZE(args) == 0);
  bool kwargs_empty =
      kwargs == nullptr || kwargs == Py_None ||
      (PyDict_Check(kwargs) && PyDict_GET_SIZE(kwargs) == 0);

  if (args_empty && kwargs_empty) {
    PyObject* result = call_one(callback, request);
    if (!result) {
      throw nb::python_error();
    }
    return nb::steal<nb::object>(result);
  }

  // Slow path: build (request, *args)
  nb::list pos;
  pos.append(nb::handle(request));
  if (args && args != Py_None) {
    PyObject* iter = PyObject_GetIter(args);
    if (!iter) {
      throw nb::python_error();
    }
    PyObject* item;
    while ((item = PyIter_Next(iter)) != nullptr) {
      pos.append(nb::steal<nb::object>(item));
    }
    Py_DECREF(iter);
    if (PyErr_Occurred()) {
      throw nb::python_error();
    }
  }
  nb::tuple call_args = nb::tuple(pos);
  PyObject* kw = g_lean.empty_dict;
  nb::dict kw_holder;
  if (kwargs && kwargs != Py_None) {
    if (PyDict_Check(kwargs)) {
      kw = kwargs;
    } else {
      PyObject* items = PyObject_CallMethod(kwargs, "items", nullptr);
      if (!items) {
        throw nb::python_error();
      }
      PyObject* it = PyObject_GetIter(items);
      Py_DECREF(items);
      if (!it) {
        throw nb::python_error();
      }
      PyObject* pair;
      while ((pair = PyIter_Next(it)) != nullptr) {
        PyObject* k = PySequence_GetItem(pair, 0);
        PyObject* v = PySequence_GetItem(pair, 1);
        Py_DECREF(pair);
        if (!k || !v) {
          Py_XDECREF(k);
          Py_XDECREF(v);
          Py_DECREF(it);
          throw nb::python_error();
        }
        kw_holder[nb::handle(k)] = nb::handle(v);
        Py_DECREF(k);
        Py_DECREF(v);
      }
      Py_DECREF(it);
      kw = kw_holder.ptr();
    }
  }
  PyObject* result = PyObject_Call(callback, call_args.ptr(), kw);
  if (!result) {
    throw nb::python_error();
  }
  return nb::steal<nb::object>(result);
}

nb::object response_for_python_error(nb::handle request, nb::python_error& e) {
  lean_cache_init();
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
  PyObject* res = PyObject_CallFunctionObjArgs(
      g_lean.response_for_exception, request.ptr(), exc.ptr(), nullptr);
  if (!res) {
    throw nb::python_error();
  }
  return nb::steal<nb::object>(res);
}

void set_attr(PyObject* obj, PyObject* name, PyObject* value) {
  if (PyObject_SetAttr(obj, name, value) < 0) {
    throw nb::python_error();
  }
}

void set_attr_none(PyObject* obj, PyObject* name) {
  if (PyObject_SetAttr(obj, name, Py_None) < 0) {
    throw nb::python_error();
  }
}

}  // namespace

bool wsgi_handler_lean_eligible(nb::handle handler) {
  try {
    lean_cache_init();
    PyObject* empty =
        PyObject_GetAttr(handler.ptr(), g_lean.name__middleware_hooks_empty);
    if (!empty) {
      PyErr_Clear();
      return false;
    }
    bool ok = truthy(empty);
    Py_DECREF(empty);
    if (!ok) {
      return false;
    }
    PyObject* fn =
        PyObject_GetAttr(handler.ptr(), g_lean.name__any_atomic_requests);
    if (!fn) {
      PyErr_Clear();
      return false;
    }
    PyObject* r = call_noargs(fn);
    Py_DECREF(fn);
    if (!r) {
      PyErr_Clear();
      return false;
    }
    ok = !truthy(r);
    Py_DECREF(r);
    return ok;
  } catch (...) {
    return false;
  }
}

bool wsgi_environ_is_lean_get(nb::handle environ_h) noexcept {
  try {
    lean_cache_init();
    PyObject* env = environ_h.ptr();
    if (!PyDict_Check(env)) {
      return false;
    }
    PyObject* method_o = env_get(env, g_lean.str_REQUEST_METHOD);
    if (!method_o) {
      return false;
    }
    std::string method = upper_ascii(py_to_std(method_o));
    if (method != "GET" && method != "HEAD") {
      return false;
    }
    PyObject* te = env_get(env, g_lean.str_HTTP_TRANSFER_ENCODING);
    if (te && te != Py_None) {
      if (!py_to_std(te).empty()) {
        return false;
      }
    }
    PyObject* cl_o = env_get(env, g_lean.str_CONTENT_LENGTH);
    if (cl_o && cl_o != Py_None) {
      std::string cl = py_to_std(cl_o);
      if (!cl.empty() && cl != "0") {
        try {
          if (std::stoll(cl) != 0) {
            return false;
          }
        } catch (...) {
          return false;
        }
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
  lean_cache_init();
  PyObject* env = environ_h.ptr();
  if (!PyDict_Check(env)) {
    return false;
  }

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
  PyObject* method_o = env_get(env, g_lean.str_REQUEST_METHOD);
  std::string method =
      upper_ascii(method_o ? py_to_std(method_o) : std::string("GET"));
  std::string path = wsgi_request_path(script_name, path_info);

  PyObject* pi_py =
      PyUnicode_FromStringAndSize(path_info.data(), path_info.size());
  PyObject* sn_py =
      PyUnicode_FromStringAndSize(script_name.data(), script_name.size());
  PyObject* path_py = PyUnicode_FromStringAndSize(path.data(), path.size());
  PyObject* method_py =
      PyUnicode_FromStringAndSize(method.data(), method.size());
  if (!pi_py || !sn_py || !path_py || !method_py) {
    Py_XDECREF(pi_py);
    Py_XDECREF(sn_py);
    Py_XDECREF(path_py);
    Py_XDECREF(method_py);
    throw nb::python_error();
  }

  if (PyDict_SetItem(env, g_lean.str_PATH_INFO, pi_py) < 0 ||
      PyDict_SetItem(env, g_lean.str_SCRIPT_NAME, sn_py) < 0) {
    Py_DECREF(pi_py);
    Py_DECREF(sn_py);
    Py_DECREF(path_py);
    Py_DECREF(method_py);
    throw nb::python_error();
  }

  PyObject* req = request.ptr();
  // Empty stream: BytesIO(b"")
  PyObject* stream = call_one(g_lean.BytesIO, g_lean.bytes_empty);
  if (!stream) {
    Py_DECREF(pi_py);
    Py_DECREF(sn_py);
    Py_DECREF(path_py);
    Py_DECREF(method_py);
    throw nb::python_error();
  }
  PyObject* content_params = PyDict_New();
  if (!content_params) {
    Py_DECREF(stream);
    Py_DECREF(pi_py);
    Py_DECREF(sn_py);
    Py_DECREF(path_py);
    Py_DECREF(method_py);
    throw nb::python_error();
  }

  set_attr(req, g_lean.name_environ, env);
  set_attr(req, g_lean.name_path_info, pi_py);
  set_attr(req, g_lean.name_path, path_py);
  set_attr(req, g_lean.name_META, env);
  set_attr(req, g_lean.name_method, method_py);
  set_attr(req, g_lean.name_content_type, g_lean.str_empty);
  set_attr(req, g_lean.name_content_params, content_params);
  set_attr(req, g_lean.name__stream, stream);
  set_attr(req, g_lean.name__read_started, Py_False);
  set_attr_none(req, g_lean.name_resolver_match);

  Py_DECREF(pi_py);
  Py_DECREF(sn_py);
  Py_DECREF(path_py);
  Py_DECREF(method_py);
  Py_DECREF(stream);
  Py_DECREF(content_params);
  return true;
}

nb::object wsgi_lean_get_response(nb::handle handler, nb::handle request) {
  lean_cache_init();
  PyObject* h = handler.ptr();
  PyObject* req = request.ptr();

  // Skip urlconf pin when load_middleware already pinned (per-worker).
  PyObject* pinned =
      PyObject_GetAttr(h, g_lean.name__lean_urlconf_pinned);
  bool already = pinned && truthy(pinned);
  Py_XDECREF(pinned);
  if (!already) {
    PyObject* ensure =
        PyObject_GetAttrString(h, "_ensure_root_urlconf");
    if (!ensure) {
      throw nb::python_error();
    }
    PyObject* r = call_noargs(ensure);
    Py_DECREF(ensure);
    if (!r) {
      throw nb::python_error();
    }
    Py_DECREF(r);
  }

  PyObject* callback = nullptr;
  PyObject* args = g_lean.empty_tuple;
  PyObject* kwargs = g_lean.empty_dict;
  bool resolved = false;
  bool decref_callback = false;
  bool decref_args = false;
  bool decref_kwargs = false;

  // Prefer pre-bound exact_callbacks (path → view) for empty-args routes.
  PyObject* path_info = PyObject_GetAttr(req, g_lean.name_path_info);
  if (path_info) {
    PyObject* cbs = PyObject_GetAttr(h, g_lean.name__exact_callbacks);
    if (cbs && cbs != Py_None && PyDict_Check(cbs)) {
      PyObject* cb = PyDict_GetItem(cbs, path_info);  // borrowed
      if (cb != nullptr) {
        callback = cb;
        args = g_lean.empty_tuple;
        kwargs = g_lean.empty_dict;
        set_attr_none(req, g_lean.name_resolver_match);
        resolved = true;
      }
    } else {
      PyErr_Clear();
    }
    Py_XDECREF(cbs);
    // Full exact_routes table (callback, args, kwargs, ...)
    if (!resolved) {
      PyObject* exact = PyObject_GetAttr(h, g_lean.name__exact_routes);
      if (exact && exact != Py_None && PyDict_Check(exact)) {
        PyObject* entry = PyDict_GetItem(exact, path_info);  // borrowed
        if (entry && PyTuple_Check(entry) && PyTuple_GET_SIZE(entry) >= 3) {
          callback = PyTuple_GET_ITEM(entry, 0);  // borrowed
          args = PyTuple_GET_ITEM(entry, 1);
          kwargs = PyTuple_GET_ITEM(entry, 2);
          set_attr_none(req, g_lean.name_resolver_match);
          resolved = true;
        }
      } else {
        PyErr_Clear();
      }
      Py_XDECREF(exact);
    }
    Py_DECREF(path_info);
  } else {
    PyErr_Clear();
  }

  if (!resolved) {
    PyObject* resolve =
        PyObject_GetAttrString(h, "resolve_request");
    if (!resolve) {
      throw nb::python_error();
    }
    PyObject* match = call_one(resolve, req);
    Py_DECREF(resolve);
    if (!match) {
      throw nb::python_error();
    }
    callback = PyObject_GetAttr(match, g_lean.name_func);
    args = PyObject_GetAttr(match, g_lean.name_args);
    kwargs = PyObject_GetAttr(match, g_lean.name_kwargs);
    Py_DECREF(match);
    if (!callback || !args || !kwargs) {
      Py_XDECREF(callback);
      Py_XDECREF(args);
      Py_XDECREF(kwargs);
      throw nb::python_error();
    }
    decref_callback = decref_args = decref_kwargs = true;
  }

  nb::object response;
  try {
    response = call_view_fast(callback, req, args, kwargs);
  } catch (...) {
    if (decref_callback) {
      Py_DECREF(callback);
    }
    if (decref_args) {
      Py_DECREF(args);
    }
    if (decref_kwargs) {
      Py_DECREF(kwargs);
    }
    throw;
  }
  if (decref_args) {
    Py_DECREF(args);
  }
  if (decref_kwargs) {
    Py_DECREF(kwargs);
  }

  // Hot path: non-None response → skip check_response (TE floor).
  // Coroutine views are not on the lean floor; treat None only.
  PyObject* resp = response.ptr();
  if (resp == Py_None) {
    PyObject* check = PyObject_GetAttrString(h, "check_response");
    if (!check) {
      if (decref_callback) {
        Py_DECREF(callback);
      }
      throw nb::python_error();
    }
    PyObject* r =
        PyObject_CallFunctionObjArgs(check, resp, callback, nullptr);
    Py_DECREF(check);
    if (decref_callback) {
      Py_DECREF(callback);
      decref_callback = false;
    }
    if (!r) {
      throw nb::python_error();
    }
    Py_DECREF(r);
  }
  if (decref_callback) {
    Py_DECREF(callback);
  }

  // TemplateResponse only — skip hasattr for normal HttpResponse via
  // looking up render; absence is common and cheap if we use GetAttr + clear.
  PyObject* render = PyObject_GetAttr(resp, g_lean.name_render);
  if (render) {
    if (PyCallable_Check(render)) {
      PyObject* rendered = call_noargs(render);
      Py_DECREF(render);
      if (!rendered) {
        throw nb::python_error();
      }
      response = nb::steal<nb::object>(rendered);
      resp = response.ptr();
    } else {
      Py_DECREF(render);
    }
  } else {
    PyErr_Clear();
  }

  // response._resource_closers.append(request.close)
  PyObject* closers =
      PyObject_GetAttr(resp, g_lean.name__resource_closers);
  if (!closers) {
    throw nb::python_error();
  }
  PyObject* close_m = PyObject_GetAttr(req, g_lean.name_close);
  if (!close_m) {
    Py_DECREF(closers);
    throw nb::python_error();
  }
  PyObject* append_m = PyObject_GetAttr(closers, g_lean.name_append);
  Py_DECREF(closers);
  if (!append_m) {
    Py_DECREF(close_m);
    throw nb::python_error();
  }
  PyObject* ar = call_one(append_m, close_m);
  Py_DECREF(append_m);
  Py_DECREF(close_m);
  if (!ar) {
    throw nb::python_error();
  }
  Py_DECREF(ar);

  // 4xx logging only
  PyObject* sc = PyObject_GetAttr(resp, g_lean.name_status_code);
  if (sc) {
    long code = PyLong_AsLong(sc);
    Py_DECREF(sc);
    if (code >= 400) {
      nb::object log_response =
          nb::module_::import_("django.utils.log").attr("log_response");
      nb::dict kw;
      kw["response"] = response;
      kw["request"] = request;
      nb::tuple log_args = nb::make_tuple(
          nb::str("%s: %s"), response.attr("reason_phrase"),
          request.attr("path"));
      nb::steal<nb::object>(
          PyObject_Call(log_response.ptr(), log_args.ptr(), kw.ptr()));
    }
  } else {
    PyErr_Clear();
  }
  return response;
}

nb::object wsgi_handler_call(nb::handle handler, nb::handle environ,
                             nb::handle start_response) {
  lean_cache_init();
  PyObject* env = environ.ptr();
  if (!PyDict_Check(env)) {
    // Fall through with nb cast errors
    env = environ.ptr();
  }

  // script prefix
  PyObject* script_name = nullptr;
  bool decref_sn = false;
  if (auto sn = script_name_fast(env)) {
    script_name =
        PyUnicode_FromStringAndSize(sn->data(), static_cast<Py_ssize_t>(sn->size()));
    if (!script_name) {
      throw nb::python_error();
    }
    decref_sn = true;
  } else {
    script_name = call_one(g_lean.get_script_name, env);
    if (!script_name) {
      throw nb::python_error();
    }
    decref_sn = true;
  }
  {
    PyObject* r = call_one(g_lean.set_script_prefix, script_name);
    if (!r) {
      if (decref_sn) {
        Py_DECREF(script_name);
      }
      throw nb::python_error();
    }
    Py_DECREF(r);
  }
  if (decref_sn) {
    Py_DECREF(script_name);
  }

  // request_started (item 3: skip if no receivers)
  PyObject* handler_type = PyObject_Type(handler.ptr());  // new ref
  if (!handler_type) {
    throw nb::python_error();
  }
  try {
    maybe_send_request_started(handler_type, env);
  } catch (...) {
    Py_DECREF(handler_type);
    throw;
  }

  // request — prefer __new__ + lean init (skip Python WSGIRequest.__init__).
  PyObject* request_class =
      PyObject_GetAttr(handler.ptr(), g_lean.name_request_class);
  if (!request_class) {
    Py_DECREF(handler_type);
    throw nb::python_error();
  }
  PyObject* request_o = nullptr;
  {
    // WSGIRequest.__new__(cls) then C++ lean populate for GET/HEAD empty body.
    PyObject* new_m = PyObject_GetAttrString(request_class, "__new__");
    if (new_m) {
      PyObject* bare = PyObject_CallFunctionObjArgs(new_m, request_class, nullptr);
      Py_DECREF(new_m);
      if (bare) {
        try {
          if (wsgi_request_try_lean_init(nb::handle(bare), environ)) {
            request_o = bare;
          } else {
            Py_DECREF(bare);
          }
        } catch (...) {
          Py_DECREF(bare);
          Py_DECREF(request_class);
          Py_DECREF(handler_type);
          throw;
        }
      } else {
        PyErr_Clear();
      }
    } else {
      PyErr_Clear();
    }
  }
  if (!request_o) {
    request_o = call_one(request_class, env);
  }
  Py_DECREF(request_class);
  if (!request_o) {
    Py_DECREF(handler_type);
    throw nb::python_error();
  }
  nb::object request = nb::steal<nb::object>(request_o);

  // Path selection (flags frozen at load_middleware):
  //   lean_view_only     → exact routes + view only (empty MIDDLEWARE)
  //   native_stock_chain → C++ stock chain around lean view (Security/XFrame/…)
  //   hybrid_flattened   → hybrid_chain_call in C++ (no Python get_response)
  //   else               → Python get_response (full middleware onion)
  nb::object response;
  bool lean_view = false;
  bool stock_chain = false;
  bool hybrid_flat = false;
  nb::object stock_specs;
  {
    PyObject* flag =
        PyObject_GetAttrString(handler.ptr(), "_lean_view_only");
    if (flag) {
      lean_view = truthy(flag);
      Py_DECREF(flag);
    } else {
      PyErr_Clear();
    }
    PyObject* sc =
        PyObject_GetAttrString(handler.ptr(), "_native_stock_chain");
    if (sc) {
      stock_chain = truthy(sc);
      Py_DECREF(sc);
    } else {
      PyErr_Clear();
    }
    if (stock_chain) {
      PyObject* specs =
          PyObject_GetAttrString(handler.ptr(), "_native_stock_specs");
      if (specs && specs != Py_None) {
        stock_specs = nb::steal<nb::object>(specs);
      } else {
        Py_XDECREF(specs);
        PyErr_Clear();
        stock_chain = false;
      }
    }
    if (!lean_view && !stock_chain) {
      PyObject* hf =
          PyObject_GetAttrString(handler.ptr(), "_hybrid_flattened");
      if (hf) {
        hybrid_flat = truthy(hf);
        Py_DECREF(hf);
      } else {
        PyErr_Clear();
      }
    }
  }
  try {
    if (lean_view) {
      response = wsgi_lean_get_response(handler, request);
    } else if (stock_chain) {
      // Pure stock: C++ security/xframe/common around lean view.
      // handler._stock_view is partial(wsgi_lean_get_response, handler)
      // set at load_middleware.
      nb::object stock_view = handler.attr("_stock_view");
      response = native_stock_chain_call(
          nb::cast<nb::sequence>(stock_specs), request, stock_view);
      response.attr("_resource_closers")
          .attr("append")(request.attr("close"));
      if (nb::cast<int>(response.attr("status_code")) >= 400) {
        nb::object log_response =
            nb::module_::import_("django.utils.log").attr("log_response");
        nb::dict kw;
        kw["response"] = response;
        kw["request"] = request;
        nb::tuple log_args = nb::make_tuple(
            nb::str("%s: %s"), response.attr("reason_phrase"),
            request.attr("path"));
        nb::steal<nb::object>(
            PyObject_Call(log_response.ptr(), log_args.ptr(), kw.ptr()));
      }
    } else if (hybrid_flat) {
      // Flattened hybrid: hybrid_chain_call + closers in C++ (skip Python
      // get_response / convert_exception_to_response on the hot path).
      nb::dict cfg = nb::cast<nb::dict>(handler.attr("_hybrid_cfg"));
      nb::dict bits = nb::cast<nb::dict>(handler.attr("_hybrid_bits"));
      nb::object inner_gr = handler.attr("_hybrid_get_response");
      response = hybrid_chain_call(cfg, bits, request, inner_gr);
      // request.close on empty cold path is still required for WSGI cleanup.
      response.attr("_resource_closers")
          .attr("append")(request.attr("close"));
      if (nb::cast<int>(response.attr("status_code")) >= 400) {
        nb::object log_response =
            nb::module_::import_("django.utils.log").attr("log_response");
        nb::dict kw;
        kw["response"] = response;
        kw["request"] = request;
        nb::tuple log_args = nb::make_tuple(
            nb::str("%s: %s"), response.attr("reason_phrase"),
            request.attr("path"));
        nb::steal<nb::object>(
            PyObject_Call(log_response.ptr(), log_args.ptr(), kw.ptr()));
      }
    } else {
      // Generic: full Python get_response (middleware onion).
      PyObject* gr =
          PyObject_GetAttrString(handler.ptr(), "get_response");
      if (!gr) {
        throw nb::python_error();
      }
      PyObject* resp = call_one(gr, request.ptr());
      Py_DECREF(gr);
      if (!resp) {
        throw nb::python_error();
      }
      response = nb::steal<nb::object>(resp);
    }
  } catch (nb::python_error& e) {
    try {
      response = response_for_python_error(request, e);
    } catch (...) {
      Py_DECREF(handler_type);
      throw;
    }
  }

  set_attr(response.ptr(), g_lean.name__handler_class, handler_type);
  Py_DECREF(handler_type);

  return wsgi_pack_start_response(response, start_response, environ);
}

nb::object wsgi_pack_start_response(nb::handle response,
                                    nb::handle start_response,
                                    nb::handle environ) {
  lean_cache_init();
  PyObject* resp = response.ptr();

  // --- status line ---------------------------------------------------------
  PyObject* status = nullptr;
  PyObject* sc = PyObject_GetAttr(resp, g_lean.name_status_code);
  long code = sc ? PyLong_AsLong(sc) : -1;
  Py_XDECREF(sc);
  if (code == 200) {
    PyObject* reason =
        PyObject_GetAttr(resp, g_lean.name__reason_phrase);
    if (reason == Py_None || reason == nullptr) {
      Py_XDECREF(reason);
      status = g_lean.str_200_ok;
      Py_INCREF(status);
    } else {
      Py_XDECREF(reason);
      PyObject* m = PyObject_GetAttr(resp, g_lean.name_wsgi_status_line);
      if (!m) {
        throw nb::python_error();
      }
      status = call_noargs(m);
      Py_DECREF(m);
      if (!status) {
        throw nb::python_error();
      }
    }
  } else {
    PyObject* m = PyObject_GetAttr(resp, g_lean.name_wsgi_status_line);
    if (!m) {
      throw nb::python_error();
    }
    status = call_noargs(m);
    Py_DECREF(m);
    if (!status) {
      throw nb::python_error();
    }
  }

  // --- headers from ResponseHeaders._store (dict of lower→(key, value)) ----
  // Avoid response.items() generator / Python Mapping ABC.
  nb::list response_headers;
  PyObject* headers_obj = PyObject_GetAttr(resp, g_lean.name_headers);
  if (!headers_obj) {
    Py_DECREF(status);
    throw nb::python_error();
  }
  PyObject* store = PyObject_GetAttr(headers_obj, g_lean.name__store);
  Py_DECREF(headers_obj);
  if (store && PyDict_Check(store)) {
    PyObject* values = PyDict_Values(store);
    if (!values) {
      Py_DECREF(store);
      Py_DECREF(status);
      throw nb::python_error();
    }
    const Py_ssize_t n = PyList_GET_SIZE(values);
    for (Py_ssize_t i = 0; i < n; ++i) {
      PyObject* pair = PyList_GET_ITEM(values, i);  // borrowed (key, value)
      if (pair && PyTuple_Check(pair) && PyTuple_GET_SIZE(pair) >= 2) {
        // _store values are (original_key, value) tuples — WSGI wants that.
        response_headers.append(nb::borrow(pair));
      }
    }
    Py_DECREF(values);
  } else {
    Py_XDECREF(store);
    PyErr_Clear();
    // Fallback: response.items()
    PyObject* items_m = PyObject_GetAttr(resp, g_lean.name_items);
    if (!items_m) {
      Py_DECREF(status);
      throw nb::python_error();
    }
    PyObject* items = call_noargs(items_m);
    Py_DECREF(items_m);
    if (!items) {
      Py_DECREF(status);
      throw nb::python_error();
    }
    PyObject* it = PyObject_GetIter(items);
    Py_DECREF(items);
    if (!it) {
      Py_DECREF(status);
      throw nb::python_error();
    }
    PyObject* item;
    while ((item = PyIter_Next(it)) != nullptr) {
      response_headers.append(nb::steal<nb::object>(item));
    }
    Py_DECREF(it);
    if (PyErr_Occurred()) {
      Py_DECREF(status);
      throw nb::python_error();
    }
  }
  Py_XDECREF(store);

  // cookies — skip when empty (common floor path)
  PyObject* cookies = PyObject_GetAttr(resp, g_lean.name_cookies);
  if (cookies) {
    Py_ssize_t n = PyObject_Size(cookies);
    if (n > 0) {
      PyObject* vals = PyObject_CallMethod(cookies, "values", nullptr);
      if (vals) {
        PyObject* vit = PyObject_GetIter(vals);
        Py_DECREF(vals);
        if (vit) {
          PyObject* c;
          while ((c = PyIter_Next(vit)) != nullptr) {
            PyObject* out = PyObject_CallMethod(c, "OutputString", nullptr);
            Py_DECREF(c);
            if (!out) {
              Py_DECREF(vit);
              Py_DECREF(cookies);
              Py_DECREF(status);
              throw nb::python_error();
            }
            response_headers.append(nb::make_tuple(
                nb::handle(g_lean.str_set_cookie),
                nb::steal<nb::object>(out)));
          }
          Py_DECREF(vit);
        }
      }
      if (PyErr_Occurred()) {
        Py_DECREF(cookies);
        Py_DECREF(status);
        throw nb::python_error();
      }
    }
    Py_DECREF(cookies);
  } else {
    PyErr_Clear();
  }

  {
    PyObject* args = PyTuple_Pack(2, status, response_headers.ptr());
    Py_DECREF(status);
    if (!args) {
      throw nb::python_error();
    }
    PyObject* r = PyObject_CallObject(start_response.ptr(), args);
    Py_DECREF(args);
    if (!r) {
      throw nb::python_error();
    }
    Py_DECREF(r);
  }

  // file_wrapper rare path
  {
    PyObject* fts = PyObject_GetAttr(resp, g_lean.name_file_to_stream);
    if (!fts) {
      PyErr_Clear();
    } else if (fts != Py_None) {
      PyObject* env = environ.ptr();
      PyObject* wrapper =
          PyDict_Check(env)
              ? PyDict_GetItem(env, g_lean.str_wsgi_file_wrapper)
              : nullptr;
      if (wrapper && wrapper != Py_None) {
        PyObject* close_m = PyObject_GetAttr(resp, g_lean.name_close);
        if (close_m) {
          if (PyObject_SetAttrString(fts, "close", close_m) < 0) {
            Py_DECREF(close_m);
            Py_DECREF(fts);
            throw nb::python_error();
          }
          Py_DECREF(close_m);
        }
        PyObject* bs = PyObject_GetAttr(resp, g_lean.name_block_size);
        PyObject* wrapped =
            PyObject_CallFunctionObjArgs(wrapper, fts, bs, nullptr);
        Py_XDECREF(bs);
        Py_DECREF(fts);
        if (!wrapped) {
          throw nb::python_error();
        }
        return nb::steal<nb::object>(wrapped);
      }
      Py_DECREF(fts);
    } else {
      Py_DECREF(fts);
    }
  }

  return nb::borrow(response);
}

}  // namespace django::native
