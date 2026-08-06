// Django native acceleration entry point (nanobind + C++26).
#include "cache.hpp"
#include "cookie.hpp"
#include "crypto.hpp"
#include "dateparse.hpp"
#include "datastructures.hpp"
#include "filters.hpp"
#include "html.hpp"
#include "http.hpp"
#include "locale.hpp"
#include "multipart.hpp"
#include "orm.hpp"
#include "request_utils.hpp"
#include "scaffold.hpp"
#include "signing.hpp"
#include "template.hpp"
#include "template_expr.hpp"
#include "text.hpp"
#include "urlize.hpp"
#include "urls.hpp"
#include "validators.hpp"
#include "wsgi_handler.hpp"
#include "middleware.hpp"

#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>

#include "nb_util.hpp"

#include <algorithm>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nb = nanobind;

namespace {

nb::object datetime_module() {
  return nb::module_::import_("datetime");
}

nb::object make_date(const django::native::ParsedDate& d) {
  return datetime_module().attr("date")(d.year, d.month, d.day);
}

nb::object make_time(const django::native::ParsedTime& t) {
  return datetime_module().attr("time")(t.hour, t.minute, t.second, t.microsecond);
}

nb::object make_datetime(const django::native::ParsedDateTime& dt) {
  nb::object tzinfo = nb::none();
  if (dt.tz_offset_minutes.has_value()) {
    // Match Django's get_fixed_timezone(offset_minutes).
    auto get_fixed =
        nb::module_::import_("django.utils.timezone").attr("get_fixed_timezone");
    tzinfo = get_fixed(dt.tz_offset_minutes.value());
  }
  return datetime_module().attr("datetime")(dt.year, dt.month, dt.day, dt.hour,
                                            dt.minute, dt.second, dt.microsecond,
                                            tzinfo);
}

nb::object make_timedelta(const django::native::ParsedDuration& d) {
  // Build via Python timedelta so days magnitude limits raise OverflowError
  // (matches pure-Python parse_duration → timedelta(**kw)).
  // total_microseconds may have overflowed for huge day counts — re-check via
  // days derived when |us| is at the limit.
  constexpr long long kMaxDays = 999999999LL;
  constexpr long long kUsPerDay = 86400LL * 1000000LL;
  const long long us = d.total_microseconds;
  // If microseconds imply more than max days, raise OverflowError.
  // Use division carefully for negative values.
  long long days_mag = (us >= 0 ? us : -us) / kUsPerDay;
  if (days_mag > kMaxDays) {
    PyErr_SetString(PyExc_OverflowError, "duration days out of range");
    throw nb::python_error();
  }
  return datetime_module().attr("timedelta")(0, 0, us);
}

}  // namespace

NB_MODULE(_native, m) {
  m.doc() = "Django native acceleration (nanobind / C++26)";

  // --- scaffold -----------------------------------------------------------
  m.def("add", &django::native::add, nb::arg("a"), nb::arg("b"),
        "Integer addition smoke test for the native scaffold.");

  m.def(
      "version",
      []() { return std::string(django::native::version()); },
      "Native extension version string (matches package version).");

  m.def(
      "cxx_standard",
      []() { return std::string(django::native::cxx_standard()); },
      "C++ standard the extension was compiled with.");

  m.def("compiler", &django::native::compiler,
        "Compiler identification string.");

  // --- html ---------------------------------------------------------------
  m.def(
      "html_escape",
      [](const std::string& text) { return django::native::html_escape(text); },
      nb::arg("text"),
      "HTML-escape &, <, >, \", and ' (Python html.escape semantics).");

  m.def(
      "escapejs",
      [](const std::string& text) { return django::native::escapejs(text); },
      nb::arg("text"),
      "Hex-encode characters for use in JavaScript strings.");

  m.def(
      "linebreaks",
      [](const std::string& value, bool autoescape) {
        return django::native::linebreaks(value, autoescape);
      },
      nb::arg("value"), nb::arg("autoescape") = false);

  m.def(
      "linebreaksbr",
      [](const std::string& value, bool autoescape) {
        return django::native::linebreaksbr(value, autoescape);
      },
      nb::arg("value"), nb::arg("autoescape") = false);

  m.def(
      "strip_tags",
      [](const std::string& value) {
        try {
          return django::native::strip_tags(value);
        } catch (const std::runtime_error& e) {
          if (std::string_view(e.what()) == "SuspiciousOperation") {
            nb::object so =
                nb::module_::import_("django.core.exceptions")
                    .attr("SuspiciousOperation");
            PyErr_SetObject(so.ptr(),
                            nb::str("strip_tags exceeded max depth").release().ptr());
            throw nb::python_error();
          }
          throw;
        }
      },
      nb::arg("value"));

  // --- text ---------------------------------------------------------------
  m.def(
      "slugify_core",
      [](const std::string& value, bool allow_unicode) {
        return django::native::slugify_core(value, allow_unicode);
      },
      nb::arg("value"), nb::arg("allow_unicode") = false,
      "Slugify after Unicode normalization (see django.native.slugify).");

  m.def(
      "get_valid_filename",
      [](const std::string& name) -> nb::object {
        auto result = django::native::get_valid_filename(name);
        if (!result.has_value()) {
          return nb::none();
        }
        return nb::cast(*result);
      },
      nb::arg("name"),
      "Sanitize a filename; returns None if empty/'.'/'..'.");

  // --- dateparse ----------------------------------------------------------
  m.def(
      "parse_date",
      [](const std::string& value) -> nb::object {
        try {
          auto result = django::native::parse_date(value);
          if (!result.has_value()) {
            return nb::none();
          }
          return make_date(*result);
        } catch (const std::invalid_argument&) {
          throw nb::value_error("invalid date");
        }
      },
      nb::arg("value"),
      "Regex-path date parse (after fromisoformat fails).");

  m.def(
      "parse_time",
      [](const std::string& value) -> nb::object {
        try {
          auto result = django::native::parse_time(value);
          if (!result.has_value()) {
            return nb::none();
          }
          return make_time(*result);
        } catch (const std::invalid_argument&) {
          throw nb::value_error("invalid time");
        }
      },
      nb::arg("value"),
      "Regex-path time parse (after fromisoformat fails).");

  m.def(
      "parse_datetime",
      [](const std::string& value) -> nb::object {
        try {
          auto result = django::native::parse_datetime(value);
          if (!result.has_value()) {
            return nb::none();
          }
          return make_datetime(*result);
        } catch (const std::invalid_argument&) {
          throw nb::value_error("invalid datetime");
        }
      },
      nb::arg("value"),
      "Regex-path datetime parse (after fromisoformat fails).");

  m.def(
      "parse_duration",
      [](const std::string& value) -> nb::object {
        try {
          auto result = django::native::parse_duration(value);
          if (!result.has_value()) {
            return nb::none();
          }
          return make_timedelta(*result);
        } catch (const std::overflow_error& e) {
          PyErr_SetString(PyExc_OverflowError, e.what());
          throw nb::python_error();
        }
      },
      nb::arg("value"),
      "Parse Django / ISO8601 / PostgreSQL duration strings.");

  // --- http / QueryDict ---------------------------------------------------
  m.def(
      "parse_qsl_utf8",
      [](const std::string& qs, nb::object max_num_fields) {
        std::optional<std::size_t> maxf;
        if (!max_num_fields.is_none()) {
          maxf = nb::cast<std::size_t>(max_num_fields);
        }
        try {
          return django::native::list_from_string_pairs(
              django::native::parse_qsl_utf8(qs, maxf));
        } catch (const std::invalid_argument& e) {
          throw nb::value_error(e.what());
        }
      },
      nb::arg("qs"), nb::arg("max_num_fields") = nb::none(),
      "parse_qsl for UTF-8 (keep_blank_values=True). Raises ValueError if "
      "max_num_fields is exceeded.");

  // --- urls / path converters ---------------------------------------------
  nb::class_<django::native::CompiledRoute>(m, "CompiledRoute");

  m.def(
      "compile_route",
      [](const std::string& route, bool is_endpoint) -> nb::object {
        auto compiled = django::native::compile_route(route, is_endpoint);
        if (!compiled.has_value()) {
          return nb::none();
        }
        return nb::cast(std::move(*compiled));
      },
      nb::arg("route"), nb::arg("is_endpoint") = false,
      "Compile a path() route using only default converters; None if custom.");

  m.def(
      "match_route",
      [](const django::native::CompiledRoute& route,
         const std::string& path) -> nb::object {
        auto matched = django::native::match_route(route, path);
        if (!matched.has_value()) {
          return nb::none();
        }
        // Build kwargs with converter types applied (int/uuid), so Python
        // avoids a second native round-trip per parameter.
        nb::dict kwargs;
        // Walk route parts in order alongside captures.
        std::size_t cap_i = 0;
        for (const auto& part : route.parts) {
          if (part.is_literal) {
            continue;
          }
          const auto& [name, value] = matched->kwargs[cap_i++];
          switch (part.kind) {
            case django::native::ConverterKind::Int:
              kwargs[name.c_str()] =
                  django::native::converter_int_to_python(value);
              break;
            case django::native::ConverterKind::Uuid:
              kwargs[name.c_str()] =
                  nb::module_::import_("uuid").attr("UUID")(value);
              break;
            default:
              kwargs[name.c_str()] = value;
              break;
          }
        }
        return nb::make_tuple(matched->remaining, kwargs);
      },
      nb::arg("route"), nb::arg("path"),
      "Match path against CompiledRoute; None or (remaining, kwargs_dict).");

  m.def(
      "converter_int_to_python",
      [](const std::string& value) {
        try {
          return django::native::converter_int_to_python(value);
        } catch (const std::invalid_argument&) {
          throw nb::value_error("invalid literal for int converter");
        }
      },
      nb::arg("value"));

  m.def(
      "converter_int_to_url",
      [](nb::handle value) {
        // Accept int or str like Django's str(value).
        if (nb::isinstance<nb::int_>(value)) {
          return django::native::converter_int_to_url(nb::cast<long long>(value));
        }
        return std::string(nb::str(value).c_str());
      },
      nb::arg("value"));

  m.def(
      "converter_uuid_to_python",
      [](const std::string& value) {
        try {
          const std::string normalized =
              django::native::converter_uuid_to_python(value);
          return nb::module_::import_("uuid").attr("UUID")(normalized);
        } catch (const std::invalid_argument&) {
          // Match uuid.UUID: raise ValueError
          throw nb::value_error("badly formed hexadecimal UUID string");
        }
      },
      nb::arg("value"));

  m.def(
      "converter_uuid_to_url",
      [](nb::handle value) {
        return django::native::converter_uuid_to_url(
            std::string(nb::str(value).c_str()));
      },
      nb::arg("value"));

  m.def(
      "converter_str_to_python",
      [](const std::string& value) {
        return django::native::converter_str_to_python(value);
      },
      nb::arg("value"));

  m.def(
      "converter_str_to_url",
      [](nb::handle value) {
        return django::native::converter_str_to_url(
            std::string(nb::str(value).c_str()));
      },
      nb::arg("value"));

  m.def(
      "converter_slug_to_python",
      [](const std::string& value) {
        try {
          return django::native::converter_slug_to_python(value);
        } catch (const std::invalid_argument&) {
          throw nb::value_error("invalid slug");
        }
      },
      nb::arg("value"));

  m.def(
      "converter_slug_to_url",
      [](nb::handle value) {
        try {
          return django::native::converter_slug_to_url(
              std::string(nb::str(value).c_str()));
        } catch (const std::invalid_argument&) {
          throw nb::value_error("invalid slug");
        }
      },
      nb::arg("value"));

  m.def(
      "converter_path_to_python",
      [](const std::string& value) {
        try {
          return django::native::converter_path_to_python(value);
        } catch (const std::invalid_argument&) {
          throw nb::value_error("invalid path");
        }
      },
      nb::arg("value"));

  m.def(
      "converter_path_to_url",
      [](nb::handle value) {
        try {
          return django::native::converter_path_to_url(
              std::string(nb::str(value).c_str()));
        } catch (const std::invalid_argument&) {
          throw nb::value_error("invalid path");
        }
      },
      nb::arg("value"));

  m.def(
      "reverse_quote",
      [](const std::string& decoded) {
        return django::native::reverse_quote(decoded);
      },
      nb::arg("decoded"),
      "quote(decoded, safe=RFC3986_SUBDELIMS+/~:@) then escape leading //.");

  // --- template lexer -----------------------------------------------------
  m.def(
      "template_tokenize",
      [](const std::string& source, bool with_position) {
        auto tokens = django::native::template_tokenize(source, with_position);
        nb::list out;
        for (const auto& t : tokens) {
          nb::object pos_start = nb::none();
          nb::object pos_end = nb::none();
          if (t.position.has_value()) {
            pos_start = nb::cast(t.position->first);
            pos_end = nb::cast(t.position->second);
          }
          out.append(nb::make_tuple(static_cast<int>(t.type), t.contents, t.lineno,
                                    pos_start, pos_end));
        }
        return out;
      },
      nb::arg("source"), nb::arg("with_position") = false,
      "Tokenize a Django template string. "
      "Returns list of (type, contents, lineno, pos_start, pos_end).");

  // --- template expressions (smart_split / Variable / FilterExpression) ---
  m.def(
      "smart_split",
      [](const std::string& text) {
        return django::native::list_from_strings(django::native::smart_split(text));
      },
      nb::arg("text"),
      "Split on spaces while preserving quoted phrases.");

  m.def(
      "unescape_string_literal",
      [](const std::string& s) -> nb::object {
        auto r = django::native::unescape_string_literal(s);
        if (!r.has_value()) {
          throw nb::value_error("Not a string literal");
        }
        return nb::cast(*r);
      },
      nb::arg("s"));

  m.def(
      "parse_variable",
      [](const std::string& var) {
        auto p = django::native::parse_variable(var);
        nb::dict d;
        d["kind"] = static_cast<int>(p.kind);
        d["translate"] = p.translate;
        d["int_value"] = p.int_value;
        d["float_value"] = p.float_value;
        d["string_value"] = p.string_value;
        d["lookups"] = django::native::list_from_strings(p.lookups);
        d["error"] = p.error;
        d["error_detail"] = p.error_detail;
        return d;
      },
      nb::arg("var"),
      "Classify a Variable token (literal / lookup / error).");

  m.def(
      "parse_filter_expression",
      [](const std::string& token) {
        try {
          auto matches = django::native::parse_filter_expression(token);
          nb::list out;
          for (const auto& m : matches) {
            nb::dict d;
            d["kind"] = static_cast<int>(m.kind);
            d["token"] = m.token;
            d["start"] = m.start;
            d["end"] = m.end;
            if (m.arg.has_value()) {
              d["arg_is_var"] = m.arg->is_var;
              d["arg_token"] = m.arg->token;
            } else {
              d["arg_is_var"] = nb::none();
              d["arg_token"] = nb::none();
            }
            out.append(d);
          }
          return out;
        } catch (const std::invalid_argument& e) {
          throw nb::value_error(e.what());
        }
      },
      nb::arg("token"),
      "Parse FilterExpression token into match dicts.");

  m.def(
      "resolve_dict_lookups",
      [](nb::handle context, const std::vector<std::string>& lookups) {
        // Fast path: nested mapping/list lookups only (no attribute/callable).
        // Returns (ok: bool, value).
        nb::object current = nb::borrow(context);
        try {
          for (const auto& bit : lookups) {
            nb::object key = nb::str(bit.c_str());
            nb::object next;
            try {
              next = current[key];
            } catch (nb::python_error& e) {
              e.restore();
              PyErr_Clear();
              try {
                const int idx = std::stoi(bit);
                next = current[nb::int_(idx)];
              } catch (...) {
                return nb::make_tuple(false, nb::none());
              }
            }
            current = std::move(next);
          }
          if (PyCallable_Check(current.ptr())) {
            return nb::make_tuple(false, nb::none());
          }
          return nb::make_tuple(true, current);
        } catch (nb::python_error& e) {
          e.restore();
          PyErr_Clear();
          return nb::make_tuple(false, nb::none());
        } catch (...) {
          return nb::make_tuple(false, nb::none());
        }
      },
      nb::arg("context"), nb::arg("lookups"),
      "Nested dict/list lookup → (ok, value).");

  // --- defaultfilters + reverse helpers -----------------------------------
  m.def("filter_addslashes",
        [](const std::string& v) { return django::native::filter_addslashes(v); },
        nb::arg("value"));
  m.def("filter_capfirst",
        [](const std::string& v) { return django::native::filter_capfirst(v); },
        nb::arg("value"));
  m.def("filter_lower",
        [](const std::string& v) { return django::native::filter_lower(v); },
        nb::arg("value"));
  m.def("filter_upper",
        [](const std::string& v) { return django::native::filter_upper(v); },
        nb::arg("value"));
  m.def(
      "filter_cut",
      [](const std::string& v, const std::string& arg) {
        return django::native::filter_cut(v, arg);
      },
      nb::arg("value"), nb::arg("arg"));
  m.def("filter_wordcount",
        [](const std::string& v) { return django::native::filter_wordcount(v); },
        nb::arg("value"));
  m.def(
      "filter_ljust",
      [](const std::string& v, int width) {
        return django::native::filter_ljust(v, width);
      },
      nb::arg("value"), nb::arg("width"));
  m.def(
      "filter_rjust",
      [](const std::string& v, int width) {
        return django::native::filter_rjust(v, width);
      },
      nb::arg("value"), nb::arg("width"));
  m.def(
      "filter_center",
      [](const std::string& v, int width) {
        return django::native::filter_center(v, width);
      },
      nb::arg("value"), nb::arg("width"));

  m.def(
      "url_quote",
      [](const std::string& value, const std::string& safe) {
        return django::native::url_quote(value, safe);
      },
      nb::arg("value"), nb::arg("safe") = "",
      "Percent-encode URL (urllib.parse.quote semantics for UTF-8 bytes).");

  m.def(
      "escape_leading_slashes",
      [](const std::string& url) {
        return django::native::escape_leading_slashes(url);
      },
      nb::arg("url"));

  m.def(
      "context_lookup",
      [](nb::sequence dicts, nb::handle key) {
        // Walk Context.dicts from top (reversed) like BaseContext.__getitem__.
        const Py_ssize_t n = PySequence_Size(dicts.ptr());
        if (n < 0) {
          throw nb::python_error();
        }
        for (Py_ssize_t i = n - 1; i >= 0; --i) {
          nb::object d = nb::steal(PySequence_GetItem(dicts.ptr(), i));
          int has = PyMapping_HasKey(d.ptr(), key.ptr());
          if (has < 0) {
            PyErr_Clear();
            // Fall back to Python "in" for non-mapping dict-like objects.
            try {
              if (!nb::cast<bool>(d.attr("__contains__")(key))) {
                continue;
              }
            } catch (nb::python_error& e) {
              e.restore();
              PyErr_Clear();
              continue;
            }
          } else if (has == 0) {
            continue;
          }
          try {
            return nb::make_tuple(true, d[key]);
          } catch (nb::python_error& e) {
            e.restore();
            PyErr_Clear();
            continue;
          }
        }
        return nb::make_tuple(false, nb::none());
      },
      nb::arg("dicts"), nb::arg("key"),
      "Context multi-dict lookup → (found, value).");

  m.def(
      "phone2numeric",
      [](const std::string& phone) { return django::native::phone2numeric(phone); },
      nb::arg("phone"));

  m.def(
      "normalize_newlines",
      [](const std::string& text) {
        return django::native::normalize_newlines(text);
      },
      nb::arg("text"));

  m.def(
      "parse_cookie",
      [](const std::string& cookie) {
        auto items = django::native::parse_cookie(cookie);
        // Last-wins dict
        nb::dict out;
        for (const auto& [k, v] : items) {
          out[k.c_str()] = v;
        }
        return out;
      },
      nb::arg("cookie"),
      "Parse Cookie header into a dict (last key wins).");

  m.def(
      "cookie_unquote",
      [](const std::string& value) { return django::native::cookie_unquote(value); },
      nb::arg("value"));

  m.def(
      "strip_spaces_between_tags",
      [](const std::string& value) {
        return django::native::strip_spaces_between_tags(value);
      },
      nb::arg("value"));

  m.def(
      "camel_case_to_spaces",
      [](const std::string& value) {
        return django::native::camel_case_to_spaces(value);
      },
      nb::arg("value"));

  m.def(
      "pluralize_suffix",
      [](bool singular, const std::string& arg) {
        return django::native::pluralize_suffix(singular, arg);
      },
      nb::arg("singular"), nb::arg("arg"));

  m.def(
      "yesno",
      [](int tri_state, const std::string& arg) {
        return django::native::yesno(tri_state, arg);
      },
      nb::arg("tri_state"), nb::arg("arg"),
      "tri_state: 1=true, 0=false, -1=None. Empty string if arg invalid.");

  m.def(
      "get_digit",
      [](nb::handle value, nb::handle arg) -> nb::object {
        try {
          const long long v = nb::cast<long long>(nb::int_(value));
          const int a = nb::cast<int>(nb::int_(arg));
          if (a < 1) {
            return nb::int_(v);
          }
          return nb::int_(django::native::get_digit(v, a));
        } catch (...) {
          return nb::borrow(value);
        }
      },
      nb::arg("value"), nb::arg("arg"));

  m.def(
      "widthratio",
      [](double value, double max_value, int max_width) {
        try {
          return django::native::widthratio(value, max_value, max_width);
        } catch (const std::overflow_error& e) {
          PyErr_SetString(PyExc_OverflowError, e.what());
          throw nb::python_error();
        }
      },
      nb::arg("value"), nb::arg("max_value"), nb::arg("max_width"));

  m.def(
      "get_mod_func",
      [](const std::string& callback) {
        auto [mod, func] = django::native::get_mod_func(callback);
        return nb::make_tuple(mod, func);
      },
      nb::arg("callback"));

  m.def(
      "iri_to_uri",
      [](nb::object iri) -> nb::object {
        if (iri.is_none()) {
          return nb::none();
        }
        // Coerce via str() so Promise/lazy and other __str__ types work.
        return nb::cast(
            django::native::iri_to_uri(nb::cast<std::string>(nb::str(iri))));
      },
      nb::arg("iri"));

  m.def(
      "uri_to_iri",
      [](nb::object uri) -> nb::object {
        if (uri.is_none()) {
          return nb::none();
        }
        // Prefer raw bytes (force_bytes from Python) so high octets survive.
        if (PyBytes_Check(uri.ptr())) {
          char* data = nullptr;
          Py_ssize_t len = 0;
          if (PyBytes_AsStringAndSize(uri.ptr(), &data, &len) < 0) {
            throw nb::python_error();
          }
          return nb::cast(django::native::uri_to_iri(
              std::string_view(data, static_cast<std::size_t>(len))));
        }
        return nb::cast(django::native::uri_to_iri(nb::cast<std::string>(uri)));
      },
      nb::arg("uri"));

  m.def(
      "escape_uri_path",
      [](const std::string& path) {
        return django::native::escape_uri_path(path);
      },
      nb::arg("path"));

  m.def(
      "filepath_to_uri",
      [](nb::object path) -> nb::object {
        if (path.is_none()) {
          return nb::none();
        }
        return nb::cast(
            django::native::filepath_to_uri(nb::cast<std::string>(path)));
      },
      nb::arg("path"));

  m.def(
      "filter_title",
      [](const std::string& value) { return django::native::filter_title(value); },
      nb::arg("value"));

  m.def(
      "filter_slice_string",
      [](const std::string& value, nb::object start, nb::object stop,
         nb::object step) -> nb::object {
        auto opt_int = [](nb::object o) -> std::optional<int> {
          if (o.is_none()) {
            return std::nullopt;
          }
          return nb::cast<int>(o);
        };
        auto result = django::native::filter_slice_string(
            value, opt_int(start), opt_int(stop), opt_int(step));
        if (!result.has_value()) {
          throw nb::value_error("slice step cannot be zero");
        }
        return nb::cast(*result);
      },
      nb::arg("value"), nb::arg("start") = nb::none(),
      nb::arg("stop") = nb::none(), nb::arg("step") = nb::none());

  m.def(
      "divisibleby",
      [](nb::handle value, nb::handle arg) -> nb::object {
        try {
          const long long v = nb::cast<long long>(nb::int_(value));
          const long long a = nb::cast<long long>(nb::int_(arg));
          return nb::bool_(django::native::divisibleby(v, a));
        } catch (const std::invalid_argument&) {
          PyErr_SetString(PyExc_ZeroDivisionError, "integer division or modulo by zero");
          throw nb::python_error();
        } catch (...) {
          // Match Python filter: let TypeError/ValueError propagate via cast
          throw;
        }
      },
      nb::arg("value"), nb::arg("arg"));

  m.def(
      "filter_add_int",
      [](nb::handle value, nb::handle arg) -> nb::object {
        try {
          const long long v = nb::cast<long long>(nb::int_(value));
          const long long a = nb::cast<long long>(nb::int_(arg));
          auto result = django::native::filter_add_int(v, a);
          if (!result.has_value()) {
            return nb::none();
          }
          return nb::int_(*result);
        } catch (...) {
          return nb::none();
        }
      },
      nb::arg("value"), nb::arg("arg"),
      "Integer add; returns None if not both ints or on overflow.");

  m.def(
      "utf8_length",
      [](const std::string& value) {
        return django::native::utf8_length(value);
      },
      nb::arg("value"));

  m.def(
      "utf8_first",
      [](const std::string& value) { return django::native::utf8_first(value); },
      nb::arg("value"));

  m.def(
      "utf8_last",
      [](const std::string& value) { return django::native::utf8_last(value); },
      nb::arg("value"));

  m.def(
      "make_list_chars",
      [](const std::string& value) {
        return django::native::list_from_strings(
            django::native::make_list_chars(value));
      },
      nb::arg("value"));

  m.def(
      "linenumbers",
      [](const std::string& value, bool autoescape) {
        return django::native::linenumbers(value, autoescape);
      },
      nb::arg("value"), nb::arg("autoescape") = true);

  m.def(
      "wordwrap",
      [](const std::string& text, int width) {
        return django::native::wordwrap(text, width);
      },
      nb::arg("text"), nb::arg("width"));

  m.def(
      "join_strings",
      [](const std::vector<std::string>& parts, const std::string& sep) {
        return django::native::join_strings(parts, sep);
      },
      nb::arg("parts"), nb::arg("sep"),
      "Join string parts with sep (escape prepared in Python).");

  m.def(
      "filter_default",
      [](nb::object value, nb::object arg) -> nb::object {
        // value or arg — Python truthiness.
        const int truth = PyObject_IsTrue(value.ptr());
        if (truth < 0) {
          throw nb::python_error();
        }
        return truth ? value : arg;
      },
      nb::arg("value").none(), nb::arg("arg").none());

  m.def(
      "filter_default_if_none",
      [](nb::object value, nb::object arg) -> nb::object {
        if (value.is_none()) {
          return arg;
        }
        return value;
      },
      nb::arg("value").none(), nb::arg("arg").none());

  m.def(
      "sequence_random",
      [](nb::handle value) -> nb::object {
        const Py_ssize_t n = PyObject_Size(value.ptr());
        if (n < 0) {
          // Not sized — try sequence protocol length.
          PyErr_Clear();
          throw nb::type_error("object has no len()");
        }
        if (n == 0) {
          // Match random.choice IndexError → filter returns "".
          return nb::str("");
        }
        // random.randrange(n)
        nb::object random_mod = nb::module_::import_("random");
        const Py_ssize_t idx =
            nb::cast<Py_ssize_t>(random_mod.attr("randrange")(n));
        nb::object item = nb::steal(PySequence_GetItem(value.ptr(), idx));
        if (!item.is_valid()) {
          throw nb::python_error();
        }
        return item;
      },
      nb::arg("value"));

  m.def(
      "dictsort",
      [](nb::object value, nb::object sort_arg, bool reverse) -> nb::object {
        // Replicate defaultfilters._property_resolver + sorted().
        // float(arg) probe: numeric-like args use itemgetter(arg).
        bool numeric = false;
        {
          nb::object probe = nb::steal(PyNumber_Float(sort_arg.ptr()));
          if (probe.is_valid()) {
            numeric = true;
          } else {
            PyErr_Clear();
          }
        }

        // Private lookup rejected when building the resolver (Python).
        if (!numeric) {
          const std::string path = nb::cast<std::string>(nb::str(sort_arg));
          if (!path.empty() &&
              (path[0] == '_' || path.find("._") != std::string::npos)) {
            return nb::str("");
          }
        }

        auto make_key = [&](nb::handle item) -> nb::object {
          if (numeric) {
            return item[sort_arg];
          }
          const std::string path = nb::cast<std::string>(nb::str(sort_arg));
          nb::object cur = nb::borrow(item);
          std::size_t start = 0;
          while (start <= path.size()) {
            const std::size_t dot = path.find('.', start);
            const std::string part = (dot == std::string::npos)
                                         ? path.substr(start)
                                         : path.substr(start, dot - start);
            if (part.empty()) {
              throw nb::attribute_error("empty path segment");
            }
            nb::object part_s = nb::str(part.c_str());
            // try __getitem__ then getattr — match Python resolve()
            nb::object next = nb::steal(
                PyObject_GetItem(cur.ptr(), part_s.ptr()));
            if (!next.is_valid()) {
              PyErr_Clear();
              next = nb::steal(PyObject_GetAttrString(cur.ptr(), part.c_str()));
              if (!next.is_valid()) {
                throw nb::python_error();
              }
            }
            cur = std::move(next);
            if (dot == std::string::npos) {
              break;
            }
            start = dot + 1;
          }
          return cur;
        };

        // Materialize to list for sorting.
        nb::list items;
        try {
          for (nb::handle it : value) {
            items.append(it);
          }
        } catch (nb::python_error& e) {
          e.restore();
          PyErr_Clear();
          return nb::str("");
        }

        const Py_ssize_t n = items.size();
        std::vector<std::pair<nb::object, Py_ssize_t>> keyed;
        keyed.reserve(static_cast<std::size_t>(n));
        try {
          for (Py_ssize_t i = 0; i < n; ++i) {
            nb::object item = items[i];
            keyed.emplace_back(make_key(item), i);
          }
        } catch (nb::python_error& e) {
          e.restore();
          PyErr_Clear();
          return nb::str("");
        } catch (const std::exception&) {
          return nb::str("");
        }

        try {
          std::stable_sort(
              keyed.begin(), keyed.end(),
              [reverse](const auto& a, const auto& b) {
                int lt = PyObject_RichCompareBool(a.first.ptr(), b.first.ptr(),
                                                  Py_LT);
                if (lt < 0) {
                  throw nb::python_error();
                }
                int eq = PyObject_RichCompareBool(a.first.ptr(), b.first.ptr(),
                                                  Py_EQ);
                if (eq < 0) {
                  throw nb::python_error();
                }
                if (eq) {
                  return reverse ? a.second > b.second : a.second < b.second;
                }
                const bool less = lt == 1;
                return reverse ? !less : less;
              });
        } catch (nb::python_error& e) {
          e.restore();
          PyErr_Clear();
          return nb::str("");
        }

        nb::list out;
        for (const auto& k : keyed) {
          out.append(items[k.second]);
        }
        return out;
      },
      nb::arg("value"), nb::arg("arg"), nb::arg("reverse") = false);

  m.def(
      "unordered_list",
      [](nb::object value, bool autoescape) -> std::string {
        auto escape_item = [autoescape](nb::handle item) -> std::string {
          if (!autoescape) {
            return nb::cast<std::string>(nb::str(item));
          }
          if (nb::hasattr(item, "__html__")) {
            return nb::cast<std::string>(item.attr("__html__")());
          }
          return django::native::html_escape(
              nb::cast<std::string>(nb::str(item)));
        };

        auto is_listy = [](nb::handle o) -> bool {
          if (PyList_Check(o.ptr()) || PyTuple_Check(o.ptr())) {
            return true;
          }
          nb::object gen_type =
              nb::module_::import_("types").attr("GeneratorType");
          return nb::isinstance(o, gen_type);
        };

        // Match defaultfilters.walk_items: next list/tuple/gen is children.
        auto walk_items =
            [&](nb::handle item_list)
            -> std::vector<std::pair<nb::object, nb::object>> {
          std::vector<std::pair<nb::object, nb::object>> pairs;
          nb::list flat;
          for (nb::handle x : item_list) {
            flat.append(x);
          }
          const Py_ssize_t n = flat.size();
          Py_ssize_t i = 0;
          while (i < n) {
            nb::object item = flat[i];
            ++i;
            if (i < n) {
              nb::object next_item = flat[i];
              if (is_listy(next_item)) {
                pairs.emplace_back(item, next_item);
                ++i;  // skip children list
                continue;
              }
            }
            pairs.emplace_back(item, nb::none());
          }
          return pairs;
        };

        std::function<std::string(nb::handle, int)> format;
        format = [&](nb::handle item_list, int tabs) -> std::string {
          const std::string indent(static_cast<std::size_t>(tabs), '\t');
          std::vector<std::string> output;
          for (auto& [item, children] : walk_items(item_list)) {
            std::string sublist;
            if (!children.is_none()) {
              sublist = "\n" + indent + "<ul>\n" + format(children, tabs + 1) +
                        "\n" + indent + "</ul>\n" + indent;
            }
            output.push_back(indent + "<li>" + escape_item(item) + sublist +
                             "</li>");
          }
          std::string joined;
          for (std::size_t i = 0; i < output.size(); ++i) {
            if (i) {
              joined += '\n';
            }
            joined += output[i];
          }
          return joined;
        };

        return format(value, 1);
      },
      nb::arg("value"), nb::arg("autoescape") = true);

  // --- forms / validators ------------------------------------------------
  m.def(
      "is_valid_slug",
      [](const std::string& value) {
        return django::native::is_valid_slug(value);
      },
      nb::arg("value"));

  m.def(
      "is_valid_integer_string",
      [](const std::string& value) {
        return django::native::is_valid_integer_string(value);
      },
      nb::arg("value"));

  m.def(
      "form_integer_to_python",
      [](nb::object value) -> nb::object {
        if (value.is_none()) {
          return nb::none();
        }
        try {
          auto result =
              django::native::form_integer_to_python(nb::cast<std::string>(nb::str(value)));
          if (!result.has_value()) {
            return nb::none();  // signal invalid — Python raises ValidationError
          }
          return nb::int_(*result);
        } catch (...) {
          return nb::none();
        }
      },
      nb::arg("value"),
      "Parse IntegerField value; returns None if invalid or empty-ish.");

  m.def(
      "is_valid_ipv4",
      [](const std::string& value) {
        return django::native::is_valid_ipv4(value);
      },
      nb::arg("value"));

  m.def(
      "is_valid_ipv6",
      [](const std::string& value) {
        return django::native::is_valid_ipv6(value);
      },
      nb::arg("value"));

  m.def(
      "is_valid_ipv46",
      [](const std::string& value) {
        return django::native::is_valid_ipv46(value);
      },
      nb::arg("value"));

  m.def(
      "is_valid_email",
      [](const std::string& value, nb::object allowlist) {
        std::vector<std::string> al;
        if (!allowlist.is_none()) {
          for (nb::handle x : allowlist) {
            al.push_back(nb::cast<std::string>(nb::str(x)));
          }
        } else {
          al.emplace_back("localhost");
        }
        return django::native::is_valid_email(value, al);
      },
      nb::arg("value"), nb::arg("allowlist") = nb::none());

  m.def(
      "has_null_characters",
      [](const std::string& value) {
        // Note: Python str cannot contain embedded NUL when passed via
        // std::string from UTF-8; check still useful for bytes-ish paths.
        return django::native::has_null_characters(value);
      },
      nb::arg("value"));

  m.def(
      "char_field_strip",
      [](const std::string& value, bool strip) {
        return django::native::char_field_strip(value, strip);
      },
      nb::arg("value"), nb::arg("strip") = true);

  // --- signing -----------------------------------------------------------
  m.def(
      "b62_encode",
      [](long long value) { return django::native::b62_encode(value); },
      nb::arg("value"));

  m.def(
      "b62_decode",
      [](const std::string& s) -> nb::object {
        auto v = django::native::b62_decode(s);
        if (!v.has_value()) {
          throw nb::value_error("invalid base62");
        }
        return nb::int_(*v);
      },
      nb::arg("s"));

  m.def(
      "signing_b64_encode",
      [](nb::bytes data) {
        std::string enc = django::native::b64_encode(
            std::string_view(data.c_str(), data.size()));
        // Django b64_encode returns bytes (ascii).
        return nb::bytes(enc.data(), enc.size());
      },
      nb::arg("data"));

  m.def(
      "signing_b64_decode",
      [](const std::string& data) -> nb::object {
        auto v = django::native::b64_decode(data);
        if (!v.has_value()) {
          throw nb::value_error("invalid base64");
        }
        return nb::bytes(v->data(), v->size());
      },
      nb::arg("data"));

  m.def(
      "constant_time_compare",
      [](nb::object a, nb::object b) {
        std::string sa, sb;
        if (PyBytes_Check(a.ptr())) {
          char* p = nullptr;
          Py_ssize_t n = 0;
          PyBytes_AsStringAndSize(a.ptr(), &p, &n);
          sa.assign(p, static_cast<std::size_t>(n));
        } else {
          sa = nb::cast<std::string>(nb::str(a));
        }
        if (PyBytes_Check(b.ptr())) {
          char* p = nullptr;
          Py_ssize_t n = 0;
          PyBytes_AsStringAndSize(b.ptr(), &p, &n);
          sb.assign(p, static_cast<std::size_t>(n));
        } else {
          sb = nb::cast<std::string>(nb::str(b));
        }
        return django::native::constant_time_compare(sa, sb);
      },
      nb::arg("a"), nb::arg("b"));

  m.def(
      "signer_sep_unsafe",
      [](const std::string& sep) {
        return django::native::signer_sep_unsafe(sep);
      },
      nb::arg("sep"));

  // --- truncate / querydict ----------------------------------------------
  m.def(
      "truncate_chars",
      [](const std::string& text, int length, const std::string& truncate_suffix) {
        return django::native::truncate_chars(text, length, truncate_suffix);
      },
      nb::arg("text"), nb::arg("length"), nb::arg("truncate_suffix") = "…");

  m.def(
      "truncate_words",
      [](const std::string& text, int length, const std::string& truncate_suffix) {
        return django::native::truncate_words(text, length, truncate_suffix);
      },
      nb::arg("text"), nb::arg("length"), nb::arg("truncate_suffix") = "…");

  m.def(
      "querydict_urlencode",
      [](nb::sequence pairs, const std::string& safe) {
        std::vector<std::pair<std::string, std::string>> vec;
        for (nb::handle item : pairs) {
          nb::tuple t = nb::cast<nb::tuple>(item);
          vec.emplace_back(nb::cast<std::string>(nb::str(t[0])),
                           nb::cast<std::string>(nb::str(t[1])));
        }
        return django::native::querydict_urlencode(vec, safe);
      },
      nb::arg("pairs"), nb::arg("safe") = "");

  m.def(
      "url_precheck",
      [](const std::string& value, int max_length) {
        // Fast rejects for URLValidator: type/length/unsafe whitespace.
        if (static_cast<int>(value.size()) > max_length) {
          return false;
        }
        for (char c : value) {
          if (c == '\t' || c == '\r' || c == '\n') {
            return false;
          }
        }
        return true;
      },
      nb::arg("value"), nb::arg("max_length"));

  m.def(
      "url_structure_precheck",
      [](const std::string& value, int max_length, const std::string& schemes_csv) {
        return django::native::url_structure_precheck(value, max_length, schemes_csv);
      },
      nb::arg("value"), nb::arg("max_length"), nb::arg("schemes_csv"));

  m.def(
      "is_valid_domain_name",
      [](const std::string& value, bool accept_idna, int max_length) {
        return django::native::is_valid_domain_name(value, accept_idna, max_length);
      },
      nb::arg("value"), nb::arg("accept_idna") = true, nb::arg("max_length") = 255);

  // --- crypto (OpenSSL) --------------------------------------------------
  m.def(
      "salted_hmac_digest",
      [](const std::string& algorithm, nb::bytes key_salt, nb::bytes secret,
         nb::bytes value) {
        auto algo = django::native::hash_algo_from_name(algorithm);
        if (!algo) {
          throw nb::value_error("unsupported hash algorithm");
        }
        try {
          std::string dig = django::native::salted_hmac_digest(
              *algo,
              std::string_view(key_salt.c_str(), key_salt.size()),
              std::string_view(secret.c_str(), secret.size()),
              std::string_view(value.c_str(), value.size()));
          return nb::bytes(dig.data(), dig.size());
        } catch (const std::exception& e) {
          throw nb::value_error(e.what());
        }
      },
      nb::arg("algorithm"), nb::arg("key_salt"), nb::arg("secret"),
      nb::arg("value"));

  m.def(
      "pbkdf2_hmac",
      [](const std::string& algorithm, nb::bytes password, nb::bytes salt,
         int iterations, int dklen) {
        auto algo = django::native::hash_algo_from_name(algorithm);
        if (!algo) {
          throw nb::value_error("unsupported hash algorithm");
        }
        try {
          std::string out = django::native::pbkdf2_hmac(
              *algo, std::string_view(password.c_str(), password.size()),
              std::string_view(salt.c_str(), salt.size()), iterations, dklen);
          return nb::bytes(out.data(), out.size());
        } catch (const std::exception& e) {
          throw nb::value_error(e.what());
        }
      },
      nb::arg("algorithm"), nb::arg("password"), nb::arg("salt"),
      nb::arg("iterations"), nb::arg("dklen") = 0);

  m.def(
      "secure_random_string",
      [](int length, const std::string& allowed_chars) {
        try {
          return django::native::secure_random_string(length, allowed_chars);
        } catch (const std::exception& e) {
          throw nb::value_error(e.what());
        }
      },
      nb::arg("length"), nb::arg("allowed_chars"));

  // --- urlize / html truncate / decimal digits / sanitize ----------------
  m.def(
      "trim_url",
      [](const std::string& url, int limit) {
        return django::native::trim_url(url, limit);
      },
      nb::arg("url"), nb::arg("limit"));

  m.def(
      "urlize_word_split",
      [](const std::string& text) {
        return django::native::list_from_strings(
            django::native::urlize_word_split(text));
      },
      nb::arg("text"));

  m.def(
      "trim_urlize_punctuation",
      [](const std::string& word) {
        auto t = django::native::trim_urlize_punctuation(word);
        return nb::make_tuple(t.lead, t.middle, t.trail);
      },
      nb::arg("word"));

  m.def(
      "urlize_is_email_simple",
      [](const std::string& value) {
        return django::native::urlize_is_email_simple(value);
      },
      nb::arg("value"));

  m.def(
      "urlize_simple_url_match",
      [](const std::string& middle) {
        return django::native::urlize_simple_url_match(middle);
      },
      nb::arg("middle"));

  m.def(
      "urlize_simple_url_2_match",
      [](const std::string& middle) {
        return django::native::urlize_simple_url_2_match(middle);
      },
      nb::arg("middle"));

  m.def(
      "truncate_chars_html",
      [](const std::string& text, int length, const std::string& suffix) {
        return django::native::truncate_chars_html(text, length, suffix);
      },
      nb::arg("text"), nb::arg("length"), nb::arg("suffix") = "…");

  m.def(
      "truncate_words_html",
      [](const std::string& text, int length, const std::string& suffix) {
        return django::native::truncate_words_html(text, length, suffix);
      },
      nb::arg("text"), nb::arg("length"), nb::arg("suffix") = "…");

  m.def(
      "clean_ipv6_address",
      [](const std::string& ip, bool unpack_ipv4, int max_length) -> nb::object {
        auto r = django::native::clean_ipv6_address(ip, unpack_ipv4, max_length);
        if (!r) {
          return nb::none();
        }
        return nb::cast(*r);
      },
      nb::arg("ip"), nb::arg("unpack_ipv4") = false, nb::arg("max_length") = 39);

  m.def(
      "decimal_digit_counts",
      [](const std::string& digits, int exponent) {
        auto c = django::native::decimal_digit_counts(digits, exponent);
        return nb::make_tuple(c.invalid, c.digits, c.decimals, c.whole_digits);
      },
      nb::arg("digits"), nb::arg("exponent"));

  m.def(
      "sanitize_separators_ascii",
      [](const std::string& value, const std::string& decimal_sep,
         const std::string& thousand_sep, bool use_thousand) {
        return django::native::sanitize_separators_ascii(value, decimal_sep,
                                                         thousand_sep, use_thousand);
      },
      nb::arg("value"), nb::arg("decimal_sep"), nb::arg("thousand_sep"),
      nb::arg("use_thousand"));

  m.def(
      "querydict_urlencode_bytes",
      [](nb::sequence pairs, const std::string& safe) {
        std::vector<std::pair<std::string, std::string>> vec;
        for (nb::handle item : pairs) {
          nb::tuple t = nb::cast<nb::tuple>(item);
          auto to_raw = [](nb::handle h) {
            if (PyBytes_Check(h.ptr())) {
              char* p = nullptr;
              Py_ssize_t n = 0;
              if (PyBytes_AsStringAndSize(h.ptr(), &p, &n) < 0) {
                throw nb::python_error();
              }
              return std::string(p, static_cast<std::size_t>(n));
            }
            // latin-1 code units as raw bytes (0-255)
            std::string s = nb::cast<std::string>(nb::str(h));
            // If this came from UTF-8 encoding of latin-1 str, wrong —
            // require bytes from Python.
            return s;
          };
          vec.emplace_back(to_raw(t[0]), to_raw(t[1]));
        }
        return django::native::querydict_urlencode_bytes(vec, safe);
      },
      nb::arg("pairs"), nb::arg("safe") = "");

  // --- locale: numberformat / dateformat / timesince / filesize ----------
  m.def(
      "format_number",
      [](const std::string& number, const std::string& decimal_sep,
         nb::object decimal_pos, nb::object grouping, const std::string& thousand_sep,
         bool use_grouping) {
        std::optional<int> dpos;
        if (!decimal_pos.is_none()) {
          dpos = nb::cast<int>(decimal_pos);
        }
        std::vector<int> intervals;
        // grouping: int or sequence
        if (PyLong_Check(grouping.ptr())) {
          const int g = nb::cast<int>(grouping);
          intervals = {g, 0};
        } else {
          for (nb::handle x : grouping) {
            intervals.push_back(nb::cast<int>(x));
          }
        }
        return django::native::format_number(number, decimal_sep, dpos, intervals,
                                             thousand_sep, use_grouping);
      },
      nb::arg("number"), nb::arg("decimal_sep"), nb::arg("decimal_pos") = nb::none(),
      nb::arg("grouping") = 0, nb::arg("thousand_sep") = "",
      nb::arg("use_grouping") = false);

  m.def(
      "php_date_format",
      [](nb::dict parts, const std::string& format_string) -> nb::object {
        django::native::DateFormatInput in;
        in.year = nb::cast<int>(parts["year"]);
        in.month = nb::cast<int>(parts["month"]);
        in.day = nb::cast<int>(parts["day"]);
        in.hour = parts.contains("hour") ? nb::cast<int>(parts["hour"]) : 0;
        in.minute = parts.contains("minute") ? nb::cast<int>(parts["minute"]) : 0;
        in.second = parts.contains("second") ? nb::cast<int>(parts["second"]) : 0;
        in.microsecond =
            parts.contains("microsecond") ? nb::cast<int>(parts["microsecond"]) : 0;
        in.has_time = parts.contains("has_time")
                          ? nb::cast<bool>(parts["has_time"])
                          : true;
        in.has_tz =
            parts.contains("has_tz") ? nb::cast<bool>(parts["has_tz"]) : false;
        in.is_aware =
            parts.contains("is_aware") ? nb::cast<bool>(parts["is_aware"]) : false;
        if (parts.contains("tz_name")) {
          in.tz_name = nb::cast<std::string>(parts["tz_name"]);
        }
        if (parts.contains("e_name")) {
          in.e_name = nb::cast<std::string>(parts["e_name"]);
        }
        if (parts.contains("utc_offset_seconds")) {
          in.utc_offset_seconds = nb::cast<int>(parts["utc_offset_seconds"]);
        }
        if (parts.contains("is_dst")) {
          in.is_dst = nb::cast<int>(parts["is_dst"]);
        }
        auto load_names = [&](const char* key, std::vector<std::string>& dest,
                              std::size_t expect) {
          if (!parts.contains(key)) {
            return;
          }
          dest.clear();
          for (nb::handle x : parts[key]) {
            dest.push_back(nb::cast<std::string>(nb::str(x)));
          }
          if (dest.size() < expect) {
            dest.resize(expect);
          }
        };
        load_names("months", in.months, 13);
        load_names("months_3", in.months_3, 13);
        load_names("months_alt", in.months_alt, 13);
        load_names("months_ap", in.months_ap, 13);
        load_names("weekdays", in.weekdays, 7);
        load_names("weekdays_abbr", in.weekdays_abbr, 7);
        if (parts.contains("am")) {
          in.am = nb::cast<std::string>(parts["am"]);
        }
        if (parts.contains("pm")) {
          in.pm = nb::cast<std::string>(parts["pm"]);
        }
        if (parts.contains("AM")) {
          in.AM = nb::cast<std::string>(parts["AM"]);
        }
        if (parts.contains("PM")) {
          in.PM = nb::cast<std::string>(parts["PM"]);
        }
        if (parts.contains("midnight")) {
          in.midnight = nb::cast<std::string>(parts["midnight"]);
        }
        if (parts.contains("noon")) {
          in.noon = nb::cast<std::string>(parts["noon"]);
        }
        std::optional<std::int64_t> unix_ts;
        if (parts.contains("unix_timestamp") &&
            !parts["unix_timestamp"].is_none()) {
          unix_ts = nb::cast<std::int64_t>(parts["unix_timestamp"]);
        }
        try {
          auto result =
              django::native::php_date_format(in, format_string, unix_ts);
          if (!result.has_value()) {
            return nb::none();
          }
          return nb::cast(*result);
        } catch (const std::invalid_argument& e) {
          throw nb::type_error(e.what());
        } catch (const std::runtime_error&) {
          // r / U fallback
          return nb::none();
        }
      },
      nb::arg("parts"), nb::arg("format_string"));

  m.def(
      "timesince_partials",
      [](nb::object d, nb::object now, int depth) {
        auto ymdhms = [](nb::handle obj, int& y, int& m, int& day, int& h, int& mi,
                         int& s) {
          y = nb::cast<int>(obj.attr("year"));
          m = nb::cast<int>(obj.attr("month"));
          day = nb::cast<int>(obj.attr("day"));
          if (nb::hasattr(obj, "hour")) {
            h = nb::cast<int>(obj.attr("hour"));
            mi = nb::cast<int>(obj.attr("minute"));
            s = nb::cast<int>(obj.attr("second"));
          } else {
            h = mi = s = 0;
          }
        };
        int y1, m1, d1, h1, mi1, s1;
        int y2, m2, d2, h2, mi2, s2;
        ymdhms(d, y1, m1, d1, h1, mi1, s1);
        ymdhms(now, y2, m2, d2, h2, mi2, s2);
        auto parts = django::native::timesince_partials(
            y1, m1, d1, h1, mi1, s1, y2, m2, d2, h2, mi2, s2, depth);
        nb::list out;
        for (const auto& [unit, count] : parts) {
          out.append(nb::make_tuple(unit, count));
        }
        return out;
      },
      nb::arg("d"), nb::arg("now"), nb::arg("depth") = 2);

  m.def(
      "avoid_wrapping",
      [](const std::string& value) {
        return django::native::avoid_wrapping(value);
      },
      nb::arg("value"));

  m.def(
      "filesize_parts",
      [](nb::handle bytes_val) -> nb::object {
        try {
          const long long b = nb::cast<long long>(nb::int_(bytes_val));
          auto p = django::native::filesize_parts(b);
          nb::dict d;
          d["negative"] = p.negative;
          d["unit"] = p.unit;
          d["abs_bytes"] = p.abs_bytes;
          d["scaled"] = p.scaled;
          return d;
        } catch (...) {
          return nb::none();
        }
      },
      nb::arg("bytes"));

  // --- remaining request utils (django.utils.http + host validation) ------
  m.def(
      "parse_http_date",
      [](const std::string& date, int current_year) -> nb::object {
        auto t = django::native::parse_http_date(date, current_year);
        if (!t.has_value()) {
          throw nb::value_error("invalid HTTP date");
        }
        return nb::int_(*t);
      },
      nb::arg("date"), nb::arg("current_year"));

  m.def(
      "http_date",
      [](nb::object epoch_seconds) {
        std::optional<double> ep;
        if (!epoch_seconds.is_none()) {
          ep = nb::cast<double>(epoch_seconds);
        }
        return django::native::http_date(ep);
      },
      nb::arg("epoch_seconds") = nb::none());

  m.def(
      "base36_to_int",
      [](const std::string& s) {
        if (s.size() > 13) {
          throw nb::value_error("Base36 input too large");
        }
        auto v = django::native::base36_to_int(s);
        if (!v.has_value()) {
          throw nb::value_error("invalid base36");
        }
        return nb::int_(*v);
      },
      nb::arg("s"));

  m.def(
      "int_to_base36",
      [](std::uint64_t i) { return django::native::int_to_base36(i); },
      nb::arg("i"));

  m.def(
      "parse_etags",
      [](const std::string& etag_str) {
        return django::native::list_from_strings(
            django::native::parse_etags(etag_str));
      },
      nb::arg("etag_str"));

  m.def(
      "quote_etag",
      [](const std::string& etag_str) {
        return django::native::quote_etag(etag_str);
      },
      nb::arg("etag_str"));

  m.def(
      "is_same_domain",
      [](const std::string& host, const std::string& pattern) {
        return django::native::is_same_domain(host, pattern);
      },
      nb::arg("host"), nb::arg("pattern"));

  m.def(
      "split_domain_port",
      [](const std::string& host) {
        auto [domain, port] = django::native::split_domain_port(host);
        return nb::make_tuple(domain, port);
      },
      nb::arg("host"));

  m.def(
      "validate_host",
      [](const std::string& host, nb::sequence allowed_hosts) {
        std::vector<std::string> hosts;
        const Py_ssize_t n = PySequence_Size(allowed_hosts.ptr());
        for (Py_ssize_t i = 0; i < n; ++i) {
          nb::object item = nb::steal(PySequence_GetItem(allowed_hosts.ptr(), i));
          hosts.push_back(nb::cast<std::string>(item));
        }
        return django::native::validate_host(host, hosts);
      },
      nb::arg("host"), nb::arg("allowed_hosts"));

  m.def(
      "content_disposition_header",
      [](bool as_attachment, nb::object filename) -> nb::object {
        std::optional<std::string_view> fn;
        std::string storage;
        if (!filename.is_none()) {
          storage = nb::cast<std::string>(filename);
          fn = storage;
        }
        auto r = django::native::content_disposition_header(as_attachment, fn);
        if (!r.has_value()) {
          return nb::none();
        }
        return nb::cast(*r);
      },
      nb::arg("as_attachment"), nb::arg("filename") = nb::none());

  m.def(
      "urlsafe_base64_encode",
      [](nb::bytes data) {
        const std::string_view v(data.c_str(), data.size());
        return django::native::urlsafe_base64_encode(v);
      },
      nb::arg("s"));

  m.def(
      "urlsafe_base64_decode",
      [](const std::string& s) {
        auto r = django::native::urlsafe_base64_decode(s);
        if (!r.has_value()) {
          throw nb::value_error("Invalid base64");
        }
        return nb::bytes(r->data(), r->size());
      },
      nb::arg("s"));

  // --- multipart / headers ------------------------------------------------
  m.def(
      "parse_header_parameters",
      [](const std::string& line, nb::object max_length) {
        std::optional<std::size_t> maxl;
        if (!max_length.is_none()) {
          maxl = nb::cast<std::size_t>(max_length);
        }
        try {
          auto hp = django::native::parse_header_parameters(line, maxl);
          nb::dict params;
          for (const auto& [k, v] : hp.params) {
            params[k.c_str()] = v;
          }
          return nb::make_tuple(hp.main_value, params);
        } catch (const std::invalid_argument& e) {
          throw nb::value_error(e.what());
        }
      },
      nb::arg("line"), nb::arg("max_length") = nb::none(),
      "Parse Content-Type style header parameters.");

  m.def(
      "parse_multipart_headers",
      [](nb::bytes header_block) {
        const std::string_view h(header_block.c_str(), header_block.size());
        auto result = django::native::parse_multipart_headers(h);
        nb::dict outdict;
        for (const auto& entry : result.headers) {
          nb::dict params;
          for (const auto& [k, v] : entry.params) {
            // Match MultiPartParser: param values stored as bytes.
            params[k.c_str()] = nb::bytes(v.data(), v.size());
          }
          outdict[entry.name.c_str()] = nb::make_tuple(entry.value, params);
        }
        return nb::make_tuple(static_cast<int>(result.type), outdict);
      },
      nb::arg("header_block"),
      "Parse multipart part headers → (type_int, {name: (value, params)}). "
      "type: 0=raw, 1=field, 2=file.");

  m.def(
      "parse_multipart_message",
      [](nb::bytes body, nb::bytes boundary) {
        const std::string_view b(body.c_str(), body.size());
        const std::string_view bnd(boundary.c_str(), boundary.size());
        auto parts = django::native::parse_multipart_message(b, bnd);
        nb::list out;
        for (const auto& part : parts) {
          nb::dict headers;
          for (const auto& entry : part.headers) {
            nb::dict params;
            for (const auto& [k, v] : entry.params) {
              params[k.c_str()] = nb::bytes(v.data(), v.size());
            }
            headers[entry.name.c_str()] = nb::make_tuple(entry.value, params);
          }
          nb::dict item;
          item["type"] = static_cast<int>(part.type);
          item["headers"] = headers;
          item["body"] = nb::bytes(part.body.data(), part.body.size());
          item["name"] = part.name;
          item["filename"] = part.filename;
          item["content_type"] = part.content_type;
          item["transfer_encoding"] = part.transfer_encoding;
          out.append(item);
        }
        return out;
      },
      nb::arg("body"), nb::arg("boundary"),
      "Parse a complete multipart body into structured parts.");

  m.def(
      "find_multipart_boundary",
      [](nb::bytes data, nb::bytes boundary) -> nb::object {
        const std::string_view d(data.c_str(), data.size());
        const std::string_view b(boundary.c_str(), boundary.size());
        auto found = django::native::find_multipart_boundary(d, b);
        if (!found.has_value()) {
          return nb::none();
        }
        return nb::make_tuple(found->first, found->second);
      },
      nb::arg("data"), nb::arg("boundary"),
      "Find multipart boundary; returns (end, next) or None.");

  m.def(
      "boundary_chunk_slice",
      [](nb::bytes data, nb::bytes boundary, std::size_t rollback) {
        auto r = django::native::boundary_chunk_slice(
            std::string_view(data.c_str(), data.size()),
            std::string_view(boundary.c_str(), boundary.size()), rollback);
        return nb::make_tuple(r.found, r.done, r.yield_end, r.unget_start);
      },
      nb::arg("data"), nb::arg("boundary"), nb::arg("rollback"));

  m.def(
      "find_header_block_end",
      [](nb::bytes chunk) -> nb::object {
        auto r = django::native::find_header_block_end(
            std::string_view(chunk.c_str(), chunk.size()));
        if (!r) {
          return nb::none();
        }
        return nb::cast(*r);
      },
      nb::arg("chunk"));

  m.def(
      "sanitize_multipart_filename",
      [](const std::string& file_name) -> nb::object {
        auto result = django::native::sanitize_multipart_filename(file_name);
        if (!result.has_value()) {
          return nb::none();
        }
        return nb::cast(*result);
      },
      nb::arg("file_name"),
      "Sanitize upload filename (after html.unescape). None if discarded.");

  m.def(
      "split_multipart_parts",
      [](nb::bytes body, nb::bytes separator) {
        const std::string_view b(body.c_str(), body.size());
        const std::string_view s(separator.c_str(), separator.size());
        auto parts = django::native::split_multipart_parts(b, s);
        nb::list out;
        for (const auto& part : parts) {
          out.append(nb::bytes(part.data(), part.size()));
        }
        return out;
      },
      nb::arg("body"), nb::arg("separator"),
      "Split a complete multipart body into raw part payloads.");

  // --- cache / conditional / Vary ------------------------------------------
  m.def(
      "cc_delim_split",
      [](const std::string& header) {
        return django::native::list_from_strings(
            django::native::cc_delim_split(header));
      },
      nb::arg("header"));

  m.def(
      "parse_cache_control",
      [](const std::string& header) {
        return django::native::list_from_string_pairs(
            django::native::parse_cache_control(header));
      },
      nb::arg("header"));

  m.def(
      "get_max_age_from_cc",
      [](const std::string& header) -> nb::object {
        auto r = django::native::get_max_age_from_cc(header);
        if (!r) {
          return nb::none();
        }
        return nb::cast(*r);
      },
      nb::arg("header"));

  m.def(
      "if_match_passes",
      [](const std::string& target_etag, const std::vector<std::string>& etags) {
        return django::native::if_match_passes(target_etag, etags);
      },
      nb::arg("target_etag"), nb::arg("etags"));

  m.def(
      "if_none_match_passes",
      [](const std::string& target_etag, const std::vector<std::string>& etags) {
        return django::native::if_none_match_passes(target_etag, etags);
      },
      nb::arg("target_etag"), nb::arg("etags"));

  m.def(
      "if_unmodified_since_passes",
      [](nb::object last_modified, std::int64_t if_unmodified_since) {
        std::optional<std::int64_t> lm;
        if (!last_modified.is_none()) {
          lm = nb::cast<std::int64_t>(last_modified);
        }
        return django::native::if_unmodified_since_passes(lm, if_unmodified_since);
      },
      nb::arg("last_modified"), nb::arg("if_unmodified_since"));

  m.def(
      "if_modified_since_passes",
      [](nb::object last_modified, std::int64_t if_modified_since) {
        std::optional<std::int64_t> lm;
        if (!last_modified.is_none()) {
          lm = nb::cast<std::int64_t>(last_modified);
        }
        return django::native::if_modified_since_passes(lm, if_modified_since);
      },
      nb::arg("last_modified"), nb::arg("if_modified_since"));

  m.def(
      "patch_vary_headers",
      [](const std::string& existing_vary, const std::vector<std::string>& newheaders) {
        return django::native::patch_vary_headers(existing_vary, newheaders);
      },
      nb::arg("existing_vary"), nb::arg("newheaders"));

  m.def(
      "has_vary_header",
      [](const std::string& vary_header, const std::string& header_query) {
        return django::native::has_vary_header(vary_header, header_query);
      },
      nb::arg("vary_header"), nb::arg("header_query"));

  m.def(
      "merge_cache_control",
      [](const std::string& existing, nb::sequence kwargs) {
        std::vector<std::tuple<std::string, std::string, bool>> vec;
        for (nb::handle item : kwargs) {
          nb::tuple t = nb::cast<nb::tuple>(item);
          vec.emplace_back(nb::cast<std::string>(t[0]), nb::cast<std::string>(t[1]),
                           nb::cast<bool>(t[2]));
        }
        return django::native::merge_cache_control(existing, vec);
      },
      nb::arg("existing"), nb::arg("kwargs"));

  // --- datastructures / forms / json_script / floatformat ------------------
  m.def(
      "mvd_last_value",
      [](const std::vector<std::string>& values) -> nb::object {
        auto r = django::native::mvd_last_value(values);
        if (r.empty_list) {
          return nb::list();  // []
        }
        return nb::cast(r.last);
      },
      nb::arg("values"));

  m.def(
      "node_add_action",
      [](const std::string& self_connector, const std::string& conn_type,
         bool data_is_node, bool data_negated, const std::string& data_connector,
         int data_len) {
        return django::native::node_add_action(self_connector, conn_type, data_is_node,
                                               data_negated, data_connector, data_len);
      },
      nb::arg("self_connector"), nb::arg("conn_type"), nb::arg("data_is_node"),
      nb::arg("data_negated"), nb::arg("data_connector"), nb::arg("data_len"));

  m.def(
      "form_add_prefix",
      [](const std::string& prefix, const std::string& field_name) {
        return django::native::form_add_prefix(prefix, field_name);
      },
      nb::arg("prefix"), nb::arg("field_name"));

  m.def(
      "form_add_initial_prefix",
      [](const std::string& prefix, const std::string& field_name) {
        return django::native::form_add_initial_prefix(prefix, field_name);
      },
      nb::arg("prefix"), nb::arg("field_name"));

  m.def(
      "pretty_name",
      [](const std::string& name) { return django::native::pretty_name(name); },
      nb::arg("name"));

  m.def(
      "form_auto_id",
      [](const std::string& auto_id, const std::string& html_name) {
        return django::native::form_auto_id(auto_id, html_name);
      },
      nb::arg("auto_id"), nb::arg("html_name"));

  m.def(
      "checkbox_bool_value",
      [](bool key_present, const std::string& value) {
        return django::native::checkbox_bool_value(key_present, value);
      },
      nb::arg("key_present"), nb::arg("value") = "");

  m.def(
      "flatatt_build",
      [](nb::sequence key_values, nb::sequence boolean_keys) {
        std::vector<std::pair<std::string, std::string>> kvs;
        for (nb::handle item : key_values) {
          nb::tuple t = nb::cast<nb::tuple>(item);
          kvs.emplace_back(nb::cast<std::string>(t[0]), nb::cast<std::string>(t[1]));
        }
        std::vector<std::string> bools;
        for (nb::handle item : boolean_keys) {
          bools.push_back(nb::cast<std::string>(item));
        }
        return django::native::flatatt_build(kvs, bools);
      },
      nb::arg("key_values"), nb::arg("boolean_keys"));

  m.def(
      "json_script_escape",
      [](const std::string& json_str) {
        return django::native::json_script_escape(json_str);
      },
      nb::arg("json_str"));

  m.def(
      "json_script_wrap",
      [](const std::string& escaped_json, const std::string& element_id) {
        return django::native::json_script_wrap(escaped_json, element_id);
      },
      nb::arg("escaped_json"), nb::arg("element_id") = "");

  m.def(
      "floatformat_simple",
      [](const std::string& decimal_str, int p) -> nb::object {
        auto r = django::native::floatformat_simple(decimal_str, p);
        if (!r) {
          return nb::none();
        }
        return nb::cast(*r);
      },
      nb::arg("decimal_str"), nb::arg("p"));

  // --- ORM / forms / response foothold (workstreams 1-4) -------------------
  m.def(
      "sql_quote_name",
      [](const std::string& name, const std::string& style) {
        return django::native::sql_quote_name(name, style);
      },
      nb::arg("name"), nb::arg("style") = "double");

  m.def(
      "where_needed_counts",
      [](const std::string& connector, int n_children) {
        auto p = django::native::where_needed_counts(connector, n_children);
        return nb::make_tuple(p.first, p.second);
      },
      nb::arg("connector"), nb::arg("n_children"));

  m.def(
      "where_combine_sql",
      [](const std::string& connector, const std::vector<std::string>& parts,
         bool negated, bool resolved) {
        return django::native::where_combine_sql(connector, parts, negated, resolved);
      },
      nb::arg("connector"), nb::arg("parts"), nb::arg("negated") = false,
      nb::arg("resolved") = false);

  m.def(
      "sql_in_placeholders",
      [](int n) { return django::native::sql_in_placeholders(n); }, nb::arg("n"));

  m.def(
      "sql_isnull_sql",
      [](bool negated) { return django::native::sql_isnull_sql(negated); },
      nb::arg("negated") = false);

  m.def(
      "sql_comparison_rhs",
      [](const std::string& lookup_name) {
        return django::native::sql_comparison_rhs(lookup_name);
      },
      nb::arg("lookup_name"));

  m.def(
      "is_form_empty_string",
      [](const std::string& value) {
        return django::native::is_form_empty_string(value);
      },
      nb::arg("value"));

  m.def(
      "field_str_has_changed",
      [](const std::string& initial, const std::string& data) {
        return django::native::field_str_has_changed(initial, data);
      },
      nb::arg("initial"), nb::arg("data"));

  m.def(
      "boolean_field_to_python",
      [](const std::string& value) {
        return django::native::boolean_field_to_python(value) != 0;
      },
      nb::arg("value"));

  m.def(
      "null_boolean_to_python",
      [](const std::string& value) -> nb::object {
        int r = django::native::null_boolean_to_python(value);
        if (r < 0) {
          return nb::none();
        }
        return nb::cast(r != 0);
      },
      nb::arg("value"));

  m.def(
      "header_key_valid",
      [](const std::string& key) { return django::native::header_key_valid(key); },
      nb::arg("key"));

  m.def(
      "header_value_no_newlines",
      [](const std::string& value) {
        return django::native::header_value_no_newlines(value);
      },
      nb::arg("value"));

  m.def(
      "charset_from_content_type",
      [](const std::string& content_type) {
        return django::native::charset_from_content_type(content_type);
      },
      nb::arg("content_type"));

  m.def(
      "path_ends_with_slash",
      [](const std::string& path) {
        return django::native::path_ends_with_slash(path);
      },
      nb::arg("path"));

  m.def(
      "force_append_slash_path",
      [](const std::string& full_path) {
        return django::native::force_append_slash_path(full_path);
      },
      nb::arg("full_path"));

  m.def(
      "serialize_header_lines",
      [](nb::sequence headers) {
        std::vector<std::pair<std::string, std::string>> vec;
        for (nb::handle item : headers) {
          nb::tuple t = nb::cast<nb::tuple>(item);
          vec.emplace_back(nb::cast<std::string>(t[0]), nb::cast<std::string>(t[1]));
        }
        return django::native::list_from_strings(
            django::native::serialize_header_lines(vec));
      },
      nb::arg("headers"));

  m.def(
      "stringformat_simple",
      [](const std::string& value, const std::string& spec) -> nb::object {
        auto r = django::native::stringformat_simple(value, spec);
        if (!r) {
          return nb::none();
        }
        return nb::cast(*r);
      },
      nb::arg("value"), nb::arg("spec"));

  m.def(
      "floatformat_ascii",
      [](const std::string& decimal_str, int p) -> nb::object {
        auto r = django::native::floatformat_ascii(decimal_str, p);
        if (!r) {
          return nb::none();
        }
        return nb::cast(*r);
      },
      nb::arg("decimal_str"), nb::arg("p"));

  // --- ORM depth / forms / sessions / cookies (workstreams 1-5) ------------
  m.def(
      "sql_join_dotted",
      [](const std::vector<std::string>& parts) {
        return django::native::sql_join_dotted(parts);
      },
      nb::arg("parts"));

  m.def(
      "sql_pattern_wrap",
      [](const std::string& value, const std::string& kind) {
        return django::native::sql_pattern_wrap(value, kind);
      },
      nb::arg("value"), nb::arg("kind"));

  m.def(
      "choice_valid_value",
      [](const std::string& text_value, const std::vector<std::string>& choice_keys) {
        return django::native::choice_valid_value(text_value, choice_keys);
      },
      nb::arg("text_value"), nb::arg("choice_keys"));

  m.def(
      "is_decimal_string",
      [](const std::string& value) {
        return django::native::is_decimal_string(value);
      },
      nb::arg("value"));

  m.def(
      "form_float_to_python",
      [](const std::string& value) -> nb::object {
        auto r = django::native::form_float_to_python(value);
        if (!r) {
          return nb::none();
        }
        return nb::cast(*r);
      },
      nb::arg("value"));

  m.def(
      "is_valid_session_key",
      [](const std::string& key, int min_length, bool check_charset) {
        return django::native::is_valid_session_key(key, min_length, check_charset);
      },
      nb::arg("key"), nb::arg("min_length") = 8, nb::arg("check_charset") = false);

  m.def(
      "is_valid_samesite",
      [](const std::string& value) {
        return django::native::is_valid_samesite(value);
      },
      nb::arg("value"));

  m.def(
      "cookie_delete_secure",
      [](const std::string& key, const std::string& samesite) {
        return django::native::cookie_delete_secure(key, samesite);
      },
      nb::arg("key"), nb::arg("samesite") = "");

  m.def(
      "cookie_max_age_seconds",
      [](double total_seconds) {
        return django::native::cookie_max_age_seconds(total_seconds);
      },
      nb::arg("total_seconds"));

  m.def(
      "signing_split",
      [](const std::string& signed_value, const std::string& sep) {
        return django::native::list_from_strings(
            django::native::signing_split(signed_value, sep));
      },
      nb::arg("signed_value"), nb::arg("sep") = ":");

  m.def(
      "signing_is_compressed",
      [](const std::string& b64_payload) {
        return django::native::signing_is_compressed(b64_payload);
      },
      nb::arg("b64_payload"));

  m.def(
      "where_child_outcome",
      [](int child_kind, bool negated, int full_needed, int empty_needed) {
        int f = full_needed;
        int e = empty_needed;
        int code = django::native::where_child_outcome(child_kind, negated, f, e);
        return nb::make_tuple(code, f, e);
      },
      nb::arg("child_kind"), nb::arg("negated"), nb::arg("full_needed"),
      nb::arg("empty_needed"));

  // --- Query / SQLCompiler depth -------------------------------------------
  m.def(
      "sql_comma_join",
      [](const std::vector<std::string>& parts) {
        return django::native::sql_comma_join(parts);
      },
      nb::arg("parts"));

  m.def(
      "sql_order_by_clause",
      [](const std::vector<std::string>& parts) {
        return django::native::sql_order_by_clause(parts);
      },
      nb::arg("parts"));

  m.def(
      "sql_group_by_clause",
      [](const std::vector<std::string>& parts) {
        return django::native::sql_group_by_clause(parts);
      },
      nb::arg("parts"));

  m.def(
      "sql_expr_as",
      [](const std::string& expr_sql, const std::string& quoted_alias) {
        return django::native::sql_expr_as(expr_sql, quoted_alias);
      },
      nb::arg("expr_sql"), nb::arg("quoted_alias") = "");

  m.def(
      "sql_limit_offset_clause",
      [](int limit, int offset) {
        // Python facade handles limit=None (offset-only / empty).
        return django::native::sql_limit_offset_clause(limit, offset);
      },
      nb::arg("limit"), nb::arg("offset") = 0);

  m.def(
      "join_promoter_effective_connector",
      [](const std::string& connector, bool negated) {
        return django::native::join_promoter_effective_connector(connector, negated);
      },
      nb::arg("connector"), nb::arg("negated"));

  m.def(
      "join_promoter_should_promote",
      [](const std::string& effective, int votes, int num_children) {
        return django::native::join_promoter_should_promote(effective, votes,
                                                            num_children);
      },
      nb::arg("effective_connector"), nb::arg("votes"), nb::arg("num_children"));

  m.def(
      "join_promoter_should_demote",
      [](const std::string& effective, int votes, int num_children) {
        return django::native::join_promoter_should_demote(effective, votes,
                                                           num_children);
      },
      nb::arg("effective_connector"), nb::arg("votes"), nb::arg("num_children"));

  m.def(
      "quote_name_is_alias",
      [](bool in_alias_map_not_table, bool in_extra_select,
         bool external_alias_not_table) {
        return django::native::quote_name_is_alias(
            in_alias_map_not_table, in_extra_select, external_alias_not_table);
      },
      nb::arg("in_alias_map_not_table"), nb::arg("in_extra_select"),
      nb::arg("external_alias_not_table"));

  m.def(
      "q_is_empty", [](int n_children) { return django::native::q_is_empty(n_children); },
      nb::arg("n_children"));

  m.def(
      "split_lookup_path",
      [](const std::string& path) {
        return django::native::list_from_strings(
            django::native::split_lookup_path(path));
      },
      nb::arg("path"));

  m.def(
      "lookup_path_head",
      [](const std::string& path) { return django::native::lookup_path_head(path); },
      nb::arg("path"));

  m.def(
      "q_combine_empty_flags",
      [](bool self_empty, bool other_empty) {
        return django::native::q_combine_empty_flags(self_empty, other_empty);
      },
      nb::arg("self_empty"), nb::arg("other_empty"));

  // --- build_filter / lookup path resolution -------------------------------
  m.def(
      "join_lookup_path",
      [](const std::vector<std::string>& parts) {
        return django::native::join_lookup_path(parts);
      },
      nb::arg("parts"));

  m.def(
      "lookup_field_parts",
      [](const std::vector<std::string>& lookup_splitted, int n_lookup_parts) {
        return django::native::list_from_strings(
            django::native::lookup_field_parts(lookup_splitted, n_lookup_parts));
      },
      nb::arg("lookup_splitted"), nb::arg("n_lookup_parts"));

  m.def(
      "lookup_or_exact",
      [](const std::vector<std::string>& lookups) {
        return django::native::list_from_strings(
            django::native::lookup_or_exact(lookups));
      },
      nb::arg("lookups"));

  m.def(
      "refs_expression_match",
      [](const std::vector<std::string>& lookup_parts,
         const std::vector<std::string>& annotation_keys) {
        auto r = django::native::refs_expression_match(lookup_parts, annotation_keys);
        auto rem = django::native::list_from_strings(std::move(r.remaining));
        if (r.annotation.empty()) {
          return nb::make_tuple(nb::none(), rem);
        }
        return nb::make_tuple(nb::cast(r.annotation), rem);
      },
      nb::arg("lookup_parts"), nb::arg("annotation_keys"));

  m.def(
      "next_numbered_alias",
      [](const std::string& prefix, int alias_map_size) {
        return django::native::next_numbered_alias(prefix, alias_map_size);
      },
      nb::arg("prefix"), nb::arg("alias_map_size"));

  m.def(
      "alias_refcount_add",
      [](int current, int amount, bool clamp_non_negative) {
        return django::native::alias_refcount_add(current, amount, clamp_non_negative);
      },
      nb::arg("current"), nb::arg("amount"), nb::arg("clamp_non_negative") = false);

  m.def(
      "alias_refcount_increased",
      [](nb::dict pre, nb::dict post) {
        std::vector<std::pair<std::string, int>> pre_v, post_v;
        for (auto item : pre) {
          pre_v.emplace_back(nb::cast<std::string>(nb::str(item.first)),
                             nb::cast<int>(item.second));
        }
        for (auto item : post) {
          post_v.emplace_back(nb::cast<std::string>(nb::str(item.first)),
                              nb::cast<int>(item.second));
        }
        return django::native::list_from_strings(
            django::native::alias_refcount_increased(pre_v, post_v));
      },
      nb::arg("pre"), nb::arg("post"));

  m.def(
      "lookup_invalid_without_field",
      [](int n_lookup_parts, int n_field_parts) {
        return django::native::lookup_invalid_without_field(n_lookup_parts,
                                                            n_field_parts);
      },
      nb::arg("n_lookup_parts"), nb::arg("n_field_parts"));

  m.def(
      "split_order_by_item",
      [](const std::string& item) {
        auto r = django::native::split_order_by_item(item);
        return nb::make_tuple(r.field, r.descending);
      },
      nb::arg("item"));

  // --- QuerySet / sessions / forms / compiler leftovers --------------------
  m.def(
      "values_list_flags",
      [](bool flat, bool named, int n_fields) {
        return django::native::values_list_flags(flat, named, n_fields);
      },
      nb::arg("flat"), nb::arg("named"), nb::arg("n_fields"));

  m.def(
      "unique_field_alias",
      [](const std::string& base, int start_counter,
         const std::vector<std::string>& existing_keys) {
        return django::native::unique_field_alias(base, start_counter, existing_keys);
      },
      nb::arg("base"), nb::arg("start_counter"), nb::arg("existing_keys"));

  m.def(
      "session_cache_key",
      [](const std::string& prefix, const std::string& session_key) {
        return django::native::session_cache_key(prefix, session_key);
      },
      nb::arg("prefix"), nb::arg("session_key"));

  m.def(
      "session_expiry_age_seconds",
      [](int cookie_age, nb::object modification_age, nb::object expiry) {
        std::optional<int> mod, exp;
        if (!modification_age.is_none()) {
          mod = nb::cast<int>(modification_age);
        }
        if (!expiry.is_none()) {
          exp = nb::cast<int>(expiry);
        }
        return django::native::session_expiry_age_seconds(cookie_age, mod, exp);
      },
      nb::arg("cookie_age"), nb::arg("modification_age") = nb::none(),
      nb::arg("expiry") = nb::none());

  m.def(
      "session_delta_seconds",
      [](std::int64_t days, std::int64_t seconds) {
        return django::native::session_delta_seconds(days, seconds);
      },
      nb::arg("days"), nb::arg("seconds"));

  m.def(
      "session_key_missing",
      [](const std::string& session_key) {
        return django::native::session_key_missing(session_key);
      },
      nb::arg("session_key"));

  m.def(
      "sql_for_update",
      [](bool no_key, bool nowait, bool skip_locked, const std::vector<std::string>& of) {
        return django::native::sql_for_update(no_key, nowait, skip_locked, of);
      },
      nb::arg("no_key") = false, nb::arg("nowait") = false,
      nb::arg("skip_locked") = false, nb::arg("of") = std::vector<std::string>{});

  m.def(
      "sql_combinator_keyword",
      [](const std::string& combinator, bool all) {
        return django::native::sql_combinator_keyword(combinator, all);
      },
      nb::arg("combinator"), nb::arg("all") = false);

  m.def(
      "sql_combinator_join",
      [](const std::string& combinator_sql, const std::vector<std::string>& parts,
         bool wrap_parens) {
        return django::native::sql_combinator_join(combinator_sql, parts, wrap_parens);
      },
      nb::arg("combinator_sql"), nb::arg("parts"), nb::arg("wrap_parens") = false);

  m.def(
      "sql_distinct_clause",
      [](const std::vector<std::string>& fields, bool allow_on) {
        return django::native::sql_distinct_clause(fields, allow_on);
      },
      nb::arg("fields"), nb::arg("allow_on") = false);

  m.def(
      "multi_choice_has_changed",
      [](const std::vector<std::string>& initial, const std::vector<std::string>& data) {
        return django::native::multi_choice_has_changed(initial, data);
      },
      nb::arg("initial"), nb::arg("data"));

  m.def(
      "json_looks_valid",
      [](const std::string& value) {
        return django::native::json_looks_valid(value);
      },
      nb::arg("value"));

  m.def(
      "sql_from_tables",
      [](const std::vector<std::string>& clauses) {
        return django::native::sql_from_tables(clauses);
      },
      nb::arg("clauses"));

  // --- QuerySet surface (#1) -----------------------------------------------
  m.def(
      "queryset_count_from_cache",
      [](bool has_cache, int cache_len) {
        return django::native::queryset_count_from_cache(has_cache, cache_len);
      },
      nb::arg("has_cache"), nb::arg("cache_len"));

  m.def(
      "queryset_exists_from_cache",
      [](bool has_cache, bool cache_nonempty) {
        return django::native::queryset_exists_from_cache(has_cache, cache_nonempty);
      },
      nb::arg("has_cache"), nb::arg("cache_nonempty"));

  m.def(
      "queryset_use_cache_for_first_last",
      [](bool has_cache, bool ordered, bool cache_nonempty) {
        return django::native::queryset_use_cache_for_first_last(
            has_cache, ordered, cache_nonempty);
      },
      nb::arg("has_cache"), nb::arg("ordered"), nb::arg("cache_nonempty"));

  m.def(
      "iterator_chunk_validate",
      [](bool chunk_size_none, int chunk_size, bool has_prefetch) {
        return django::native::iterator_chunk_validate(chunk_size_none, chunk_size,
                                                       has_prefetch);
      },
      nb::arg("chunk_size_none"), nb::arg("chunk_size"), nb::arg("has_prefetch"));

  m.def(
      "iterator_chunk_size_or_default",
      [](bool chunk_size_none, int chunk_size, int default_size) {
        return django::native::iterator_chunk_size_or_default(
            chunk_size_none, chunk_size, default_size);
      },
      nb::arg("chunk_size_none"), nb::arg("chunk_size"),
      nb::arg("default_size") = 2000);

  m.def(
      "in_bulk_empty",
      [](bool id_list_is_none, int id_list_len) {
        return django::native::in_bulk_empty(id_list_is_none, id_list_len);
      },
      nb::arg("id_list_is_none"), nb::arg("id_list_len"));

  m.def(
      "in_bulk_filter_key",
      [](const std::string& field_name) {
        return django::native::in_bulk_filter_key(field_name);
      },
      nb::arg("field_name"));

  m.def(
      "in_bulk_batch_ranges",
      [](int n_ids, int batch_size) {
        auto ranges = django::native::in_bulk_batch_ranges(n_ids, batch_size);
        nb::list out;
        for (const auto& r : ranges) {
          out.append(nb::make_tuple(r.start, r.end));
        }
        return out;
      },
      nb::arg("n_ids"), nb::arg("batch_size"));

  m.def(
      "get_result_kind",
      [](int num_results, int limit) {
        return django::native::get_result_kind(num_results, limit);
      },
      nb::arg("num_results"), nb::arg("limit") = 0);

  // --- bulk SQL / QuerySet write guards ------------------------------------
  m.def(
      "bulk_insert_sql",
      [](const std::vector<std::string>& row_sqls) {
        return django::native::bulk_insert_sql(row_sqls);
      },
      nb::arg("row_sqls"));

  m.def(
      "bulk_placeholder_row",
      [](const std::vector<std::string>& cols) {
        return django::native::bulk_placeholder_row(cols);
      },
      nb::arg("cols"));

  m.def(
      "validate_positive_batch_size",
      [](bool is_none, int batch_size) {
        return django::native::validate_positive_batch_size(is_none, batch_size);
      },
      nb::arg("is_none"), nb::arg("batch_size") = 0);

  m.def(
      "effective_batch_size",
      [](bool user_set, int user_batch, int max_batch, int n_objs) {
        return django::native::effective_batch_size(user_set, user_batch, max_batch,
                                                    n_objs);
      },
      nb::arg("user_set"), nb::arg("user_batch"), nb::arg("max_batch"),
      nb::arg("n_objs"));

  m.def(
      "queryset_write_guard",
      [](bool combinator, bool is_sliced, bool has_distinct_fields,
         bool has_values_fields) {
        return django::native::queryset_write_guard(
            combinator, is_sliced, has_distinct_fields, has_values_fields);
      },
      nb::arg("combinator"), nb::arg("is_sliced"), nb::arg("has_distinct_fields"),
      nb::arg("has_values_fields"));

  m.def(
      "sql_update_set_clause",
      [](const std::vector<std::string>& assignments) {
        return django::native::sql_update_set_clause(assignments);
      },
      nb::arg("assignments"));

  // --- batched insert / deletion / get_or_create bookkeeping ---------------
  m.def(
      "multi_batch_needs_atomic",
      [](int n_batches) {
        return django::native::multi_batch_needs_atomic(n_batches);
      },
      nb::arg("n_batches"));

  m.def(
      "key_has_lookup_sep",
      [](const std::string& key) {
        return django::native::key_has_lookup_sep(key);
      },
      nb::arg("key"));

  m.def(
      "keys_without_lookup_sep",
      [](const std::vector<std::string>& keys) {
        return django::native::list_from_strings(
            django::native::keys_without_lookup_sep(keys));
      },
      nb::arg("keys"));

  m.def(
      "create_defaults_use_update",
      [](bool create_defaults_is_none) {
        return django::native::create_defaults_use_update(create_defaults_is_none);
      },
      nb::arg("create_defaults_is_none"));

  // --- workstreams 1-6 -----------------------------------------------------
  m.def(
      "join_sorted_comma",
      [](const std::vector<std::string>& names) {
        return django::native::join_sorted_comma(names);
      },
      nb::arg("names"));

  m.def(
      "bulk_create_conflict_kind",
      [](bool ignore_conflicts, bool update_conflicts) {
        return django::native::bulk_create_conflict_kind(ignore_conflicts,
                                                         update_conflicts);
      },
      nb::arg("ignore_conflicts"), nb::arg("update_conflicts"));

  m.def(
      "contains_preflight",
      [](bool has_values_fields, bool pk_set) {
        return django::native::contains_preflight(has_values_fields, pk_set);
      },
      nb::arg("has_values_fields"), nb::arg("pk_set"));

  m.def(
      "aggregate_distinct_fields_error",
      [](bool has_distinct_fields) {
        return django::native::aggregate_distinct_fields_error(has_distinct_fields);
      },
      nb::arg("has_distinct_fields"));

  m.def(
      "filter_after_slice_error",
      [](bool has_filters, bool is_sliced) {
        return django::native::filter_after_slice_error(has_filters, is_sliced);
      },
      nb::arg("has_filters"), nb::arg("is_sliced"));

  m.def(
      "prohibited_filter_kwargs",
      [](const std::vector<std::string>& keys) {
        return django::native::list_from_strings(
            django::native::prohibited_filter_kwargs(keys));
      },
      nb::arg("keys"));

  m.def(
      "select_for_update_options_conflict",
      [](bool nowait, bool skip_locked) {
        return django::native::select_for_update_options_conflict(nowait,
                                                                  skip_locked);
      },
      nb::arg("nowait"), nb::arg("skip_locked"));

  m.def(
      "union_empty_self_kind",
      [](int nonempty_other_count) {
        return django::native::union_empty_self_kind(nonempty_other_count);
      },
      nb::arg("nonempty_other_count"));

  m.def(
      "combinator_return_empty_self",
      [](bool self_is_empty) {
        return django::native::combinator_return_empty_self(self_is_empty);
      },
      nb::arg("self_is_empty"));

  m.def(
      "save_force_conflict",
      [](bool force_insert, bool force_update, bool has_update_fields) {
        return django::native::save_force_conflict(force_insert, force_update,
                                                   has_update_fields);
      },
      nb::arg("force_insert"), nb::arg("force_update"),
      nb::arg("has_update_fields"));

  m.def(
      "save_skip_empty_update_fields",
      [](bool update_fields_is_none, int n_update_fields) {
        return django::native::save_skip_empty_update_fields(update_fields_is_none,
                                                             n_update_fields);
      },
      nb::arg("update_fields_is_none"), nb::arg("n_update_fields"));

  m.def(
      "save_force_update_no_pk",
      [](bool pk_set, bool force_update, bool has_update_fields) {
        return django::native::save_force_update_no_pk(pk_set, force_update,
                                                       has_update_fields);
      },
      nb::arg("pk_set"), nb::arg("force_update"), nb::arg("has_update_fields"));

  m.def(
      "collector_add_empty",
      [](int n_objs) { return django::native::collector_add_empty(n_objs); },
      nb::arg("n_objs"));

  m.def(
      "collector_delete_empty",
      [](int n_models, int n_fast_deletes) {
        return django::native::collector_delete_empty(n_models, n_fast_deletes);
      },
      nb::arg("n_models"), nb::arg("n_fast_deletes"));

  m.def(
      "collector_single_fast_path",
      [](int n_models, int n_instances) {
        return django::native::collector_single_fast_path(n_models, n_instances);
      },
      nb::arg("n_models"), nb::arg("n_instances"));

  m.def(
      "can_fast_delete_result",
      [](bool from_field_blocks, bool model_ok, bool has_signal_listeners,
         bool parents_ok, bool relations_ok, bool no_bulk_related) {
        return django::native::can_fast_delete_result(
            from_field_blocks, model_ok, has_signal_listeners, parents_ok,
            relations_ok, no_bulk_related);
      },
      nb::arg("from_field_blocks"), nb::arg("model_ok"),
      nb::arg("has_signal_listeners"), nb::arg("parents_ok"),
      nb::arg("relations_ok"), nb::arg("no_bulk_related"));

  m.def(
      "sql_assignment",
      [](const std::string& quoted_col, const std::string& rhs) {
        return django::native::sql_assignment(quoted_col, rhs);
      },
      nb::arg("quoted_col"), nb::arg("rhs"));

  m.def(
      "sql_null_assignment",
      [](const std::string& quoted_col) {
        return django::native::sql_null_assignment(quoted_col);
      },
      nb::arg("quoted_col"));

  m.def(
      "sql_parenthesized_list",
      [](const std::vector<std::string>& cols) {
        return django::native::sql_parenthesized_list(cols);
      },
      nb::arg("cols"));

  m.def(
      "sql_values_row",
      [](const std::string& placeholders) {
        return django::native::sql_values_row(placeholders);
      },
      nb::arg("placeholders"));

  m.def(
      "sql_aggregate_subquery",
      [](const std::string& select_sql, const std::string& inner_sql) {
        return django::native::sql_aggregate_subquery(select_sql, inner_sql);
      },
      nb::arg("select_sql"), nb::arg("inner_sql"));

  m.def(
      "sql_space_join",
      [](const std::vector<std::string>& parts) {
        return django::native::sql_space_join(parts);
      },
      nb::arg("parts"));

  m.def(
      "row_count_or_zero",
      [](bool is_none, int row_count) {
        return django::native::row_count_or_zero(is_none, row_count);
      },
      nb::arg("is_none"), nb::arg("row_count") = 0);

  // --- workstreams 1-6 (chain / prefetch / dates / save / clean / utils) ---
  m.def("queryset_sliced_error",
        [](bool is_sliced) {
          return django::native::queryset_sliced_error(is_sliced);
        },
        nb::arg("is_sliced"));
  m.def("clear_none_arg",
        [](bool single_none) {
          return django::native::clear_none_arg(single_none);
        },
        nb::arg("single_none"));
  m.def("only_none_arg_error",
        [](bool single_none) {
          return django::native::only_none_arg_error(single_none);
        },
        nb::arg("single_none"));
  m.def("reverse_standard_ordering",
        [](bool standard_ordering) {
          return django::native::reverse_standard_ordering(standard_ordering);
        },
        nb::arg("standard_ordering"));
  m.def("queryset_index_validate",
        [](bool is_int, bool is_slice, bool has_negative) {
          return django::native::queryset_index_validate(is_int, is_slice,
                                                         has_negative);
        },
        nb::arg("is_int"), nb::arg("is_slice"), nb::arg("has_negative"));
  m.def("qs_and_empty_kind",
        [](bool self_empty, bool other_empty) {
          return django::native::qs_and_empty_kind(self_empty, other_empty);
        },
        nb::arg("self_empty"), nb::arg("other_empty"));
  m.def("qs_or_empty_kind",
        [](bool self_empty, bool other_empty) {
          return django::native::qs_or_empty_kind(self_empty, other_empty);
        },
        nb::arg("self_empty"), nb::arg("other_empty"));
  m.def("date_kind_valid",
        [](const std::string& kind) {
          return django::native::date_kind_valid(kind);
        },
        nb::arg("kind"));
  m.def("datetime_kind_valid",
        [](const std::string& kind) {
          return django::native::datetime_kind_valid(kind);
        },
        nb::arg("kind"));
  m.def("date_order_valid",
        [](const std::string& order) {
          return django::native::date_order_valid(order);
        },
        nb::arg("order"));
  m.def("order_by_desc_prefix",
        [](const std::string& order) {
          return django::native::order_by_desc_prefix(order);
        },
        nb::arg("order"));
  m.def("earliest_missing_fields",
        [](bool has_fields, bool has_get_latest_by) {
          return django::native::earliest_missing_fields(has_fields,
                                                         has_get_latest_by);
        },
        nb::arg("has_fields"), nb::arg("has_get_latest_by"));
  m.def("save_base_needs_atomic",
        [](bool has_parents) {
          return django::native::save_base_needs_atomic(has_parents);
        },
        nb::arg("has_parents"));
  m.def("save_created_flag",
        [](bool updated) {
          return django::native::save_created_flag(updated);
        },
        nb::arg("updated"));
  m.def("do_update_empty_values_kind",
        [](bool has_update_fields, bool exists) {
          return django::native::do_update_empty_values_kind(has_update_fields,
                                                             exists);
        },
        nb::arg("has_update_fields"), nb::arg("exists"));
  m.def("clean_field_skip",
        [](bool name_in_exclude, bool generated) {
          return django::native::clean_field_skip(name_in_exclude, generated);
        },
        nb::arg("name_in_exclude"), nb::arg("generated"));
  m.def("clean_field_skip_blank_empty",
        [](bool blank, bool in_empty_values) {
          return django::native::clean_field_skip_blank_empty(blank,
                                                              in_empty_values);
        },
        nb::arg("blank"), nb::arg("in_empty_values"));
  m.def("validation_has_errors",
        [](int n_error_keys) {
          return django::native::validation_has_errors(n_error_keys);
        },
        nb::arg("n_error_keys"));
  m.def("is_non_field_errors_key",
        [](const std::string& name) {
          return django::native::is_non_field_errors_key(name);
        },
        nb::arg("name"));
  m.def("fixed_timezone_name",
        [](int offset_minutes) {
          return django::native::fixed_timezone_name(offset_minutes);
        },
        nb::arg("offset_minutes"));
  m.def("datetime_is_aware",
        [](bool utcoffset_not_none) {
          return django::native::datetime_is_aware(utcoffset_not_none);
        },
        nb::arg("utcoffset_not_none"));
  m.def("datetime_is_naive",
        [](bool utcoffset_is_none) {
          return django::native::datetime_is_naive(utcoffset_is_none);
        },
        nb::arg("utcoffset_is_none"));
  m.def("mark_safe_kind",
        [](bool has_html, bool is_callable) {
          return django::native::mark_safe_kind(has_html, is_callable);
        },
        nb::arg("has_html"), nb::arg("is_callable"));
  m.def("lookup_head",
        [](const std::string& lookup) {
          return django::native::lookup_head(lookup);
        },
        nb::arg("lookup"));

  // --- full set 1-6 --------------------------------------------------------
  m.def(
      "queryset_is_ordered",
      [](bool is_empty_qs, bool has_extra_order, bool has_order_by,
         bool default_ordering, bool has_meta_ordering, bool has_group_by) {
        return django::native::queryset_is_ordered(
            is_empty_qs, has_extra_order, has_order_by, default_ordering,
            has_meta_ordering, has_group_by);
      },
      nb::arg("is_empty_qs"), nb::arg("has_extra_order"), nb::arg("has_order_by"),
      nb::arg("default_ordering"), nb::arg("has_meta_ordering"),
      nb::arg("has_group_by"));
  m.def("annotation_alias_conflicts",
        [](bool alias_in_names) {
          return django::native::annotation_alias_conflicts(alias_in_names);
        },
        nb::arg("alias_in_names"));
  m.def("complex_filter_is_q",
        [](bool is_q_instance) {
          return django::native::complex_filter_is_q(is_q_instance);
        },
        nb::arg("is_q_instance"));
  m.def("using_is_none",
        [](bool using_is_none) {
          return django::native::using_is_none(using_is_none);
        },
        nb::arg("using_is_none"));
  m.def("refresh_fields_empty",
        [](int n_fields) {
          return django::native::refresh_fields_empty(n_fields);
        },
        nb::arg("n_fields"));
  m.def(
      "refresh_fields_have_lookup_sep",
      [](const std::vector<std::string>& fields) {
        return django::native::refresh_fields_have_lookup_sep(fields);
      },
      nb::arg("fields"));
  m.def(
      "unique_check_excluded",
      [](const std::vector<std::string>& check_names,
         const std::vector<std::string>& exclude) {
        return django::native::unique_check_excluded(check_names, exclude);
      },
      nb::arg("check_names"), nb::arg("exclude"));
  m.def(
      "unique_lookup_skip_value",
      [](bool is_none, bool is_empty_str, bool empty_as_null) {
        return django::native::unique_lookup_skip_value(is_none, is_empty_str,
                                                        empty_as_null);
      },
      nb::arg("is_none"), nb::arg("is_empty_str"), nb::arg("empty_as_null"));
  m.def("unique_check_incomplete",
        [](int n_check, int n_kwargs) {
          return django::native::unique_check_incomplete(n_check, n_kwargs);
        },
        nb::arg("n_check"), nb::arg("n_kwargs"));
  m.def("unique_error_is_single_field",
        [](int n_check) {
          return django::native::unique_error_is_single_field(n_check);
        },
        nb::arg("n_check"));
  m.def("in_lookup_empty",
        [](int n_rhs) { return django::native::in_lookup_empty(n_rhs); },
        nb::arg("n_rhs"));
  m.def(
      "sql_lhs_rhs",
      [](const std::string& lhs, const std::string& rhs_op) {
        return django::native::sql_lhs_rhs(lhs, rhs_op);
      },
      nb::arg("lhs"), nb::arg("rhs_op"));
  m.def(
      "sql_or_join",
      [](const std::vector<std::string>& parts) {
        return django::native::sql_or_join(parts);
      },
      nb::arg("parts"));
  m.def(
      "is_password_usable",
      [](bool encoded_is_none, bool starts_with_unusable) {
        return django::native::is_password_usable(encoded_is_none,
                                                  starts_with_unusable);
      },
      nb::arg("encoded_is_none"), nb::arg("starts_with_unusable"));
  m.def(
      "identify_hasher_kind",
      [](int encoded_len, bool has_dollar, bool starts_md5_dollar,
         bool starts_sha1_dollar) {
        return django::native::identify_hasher_kind(
            encoded_len, has_dollar, starts_md5_dollar, starts_sha1_dollar);
      },
      nb::arg("encoded_len"), nb::arg("has_dollar"), nb::arg("starts_md5_dollar"),
      nb::arg("starts_sha1_dollar"));
  m.def(
      "hasher_algorithm_prefix",
      [](const std::string& encoded) {
        return django::native::hasher_algorithm_prefix(encoded);
      },
      nb::arg("encoded"));
  m.def(
      "cache_default_key",
      [](const std::string& key_prefix, int version, const std::string& key) {
        return django::native::cache_default_key(key_prefix, version, key);
      },
      nb::arg("key_prefix"), nb::arg("version"), nb::arg("key"));
  m.def(
      "cache_timeout_kind",
      [](bool is_default_sentinel, bool is_none, int timeout) {
        return django::native::cache_timeout_kind(is_default_sentinel, is_none,
                                                  timeout);
      },
      nb::arg("is_default_sentinel"), nb::arg("is_none"), nb::arg("timeout") = 0);
  m.def(
      "file_multiple_chunks",
      [](std::int64_t size, std::int64_t chunk_size) {
        return django::native::file_multiple_chunks(size, chunk_size);
      },
      nb::arg("size"), nb::arg("chunk_size"));
  m.def(
      "mask_hash",
      [](const std::string& hash, int show, char mask_char) {
        return django::native::mask_hash(hash, show, mask_char);
      },
      nb::arg("hash"), nb::arg("show") = 6, nb::arg("mask_char") = '*');

  // --- full set: QS / CSRF / security / URLs / template / i18n / schema ----
  m.def("result_cache_populated",
        [](bool cache_is_none) {
          return django::native::result_cache_populated(cache_is_none);
        },
        nb::arg("cache_is_none"));
  m.def("prefetch_still_needed",
        [](bool has_lookups, bool prefetch_done) {
          return django::native::prefetch_still_needed(has_lookups, prefetch_done);
        },
        nb::arg("has_lookups"), nb::arg("prefetch_done"));
  m.def("queryset_cache_truthy",
        [](int cache_len) {
          return django::native::queryset_cache_truthy(cache_len);
        },
        nb::arg("cache_len"));
  m.def("sticky_filter_active",
        [](bool sticky) {
          return django::native::sticky_filter_active(sticky);
        },
        nb::arg("sticky"));
  m.def("csrf_token_length_ok",
        [](int len, int secret_len, int token_len) {
          return django::native::csrf_token_length_ok(len, secret_len, token_len);
        },
        nb::arg("len"), nb::arg("secret_len"), nb::arg("token_len"));
  m.def("csrf_token_chars_valid",
        [](const std::string& token) {
          return django::native::csrf_token_chars_valid(token);
        },
        nb::arg("token"));
  m.def("csrf_unmask_token",
        [](const std::string& token, int secret_len) {
          return django::native::csrf_unmask_token(token, secret_len);
        },
        nb::arg("token"), nb::arg("secret_len"));
  m.def("csrf_mask_secret",
        [](const std::string& secret, const std::string& mask) {
          return django::native::csrf_mask_secret(secret, mask);
        },
        nb::arg("secret"), nb::arg("mask"));
  m.def("hsts_header_value",
        [](int seconds, bool include_subdomains, bool preload) {
          return django::native::hsts_header_value(seconds, include_subdomains,
                                                   preload);
        },
        nb::arg("seconds"), nb::arg("include_subdomains"), nb::arg("preload"));
  m.def("https_redirect_url",
        [](const std::string& host, const std::string& full_path) {
          return django::native::https_redirect_url(host, full_path);
        },
        nb::arg("host"), nb::arg("full_path"));
  m.def("referrer_policy_header",
        [](const std::vector<std::string>& policies) {
          return django::native::referrer_policy_header(policies);
        },
        nb::arg("policies"));
  m.def("route_looks_like_regex",
        [](const std::string& route) {
          return django::native::route_looks_like_regex(route);
        },
        nb::arg("route"));
  m.def(
      "route_simple_match",
      [](bool is_endpoint, const std::string& route, const std::string& path) {
        auto m = django::native::route_simple_match(is_endpoint, route, path);
        return nb::make_tuple(m.kind, m.remaining);
      },
      nb::arg("is_endpoint"), nb::arg("route"), nb::arg("path"));
  m.def("engine_loaders_app_dirs_conflict",
        [](bool app_dirs, bool loaders_defined) {
          return django::native::engine_loaders_app_dirs_conflict(app_dirs,
                                                                  loaders_defined);
        },
        nb::arg("app_dirs"), nb::arg("loaders_defined"));
  m.def("template_cache_key_plain",
        [](const std::string& template_name) {
          return django::native::template_cache_key_plain(template_name);
        },
        nb::arg("template_name"));
  m.def("to_language",
        [](const std::string& locale) {
          return django::native::to_language(locale);
        },
        nb::arg("locale"));
  m.def("to_locale",
        [](const std::string& language) {
          return django::native::to_locale(language);
        },
        nb::arg("language"));
  m.def("plural_index_default",
        [](int n) { return django::native::plural_index_default(n); },
        nb::arg("n"));
  m.def("language_code_too_long",
        [](int len, int max_len) {
          return django::native::language_code_too_long(len, max_len);
        },
        nb::arg("len"), nb::arg("max_len"));
  m.def("sql_create_table",
        [](const std::string& quoted_table, const std::string& columns_sql) {
          return django::native::sql_create_table(quoted_table, columns_sql);
        },
        nb::arg("quoted_table"), nb::arg("columns_sql"));
  m.def("migration_describe",
        [](const std::string& class_name, const std::string& constructor_args) {
          return django::native::migration_describe(class_name, constructor_args);
        },
        nb::arg("class_name"), nb::arg("constructor_args"));
  m.def("migration_formatted_description",
        [](const std::string& category, const std::string& description) {
          return django::native::migration_formatted_description(category,
                                                                 description);
        },
        nb::arg("category"), nb::arg("description"));

  // --- full menu 1-12 ------------------------------------------------------
  m.def("http_status_code_valid",
        [](int code) { return django::native::http_status_code_valid(code); },
        nb::arg("code"));
  m.def("weak_etag_if_strong",
        [](const std::string& etag) {
          return django::native::weak_etag_if_strong(etag);
        },
        nb::arg("etag"));
  m.def("accepts_gzip",
        [](const std::string& ae) { return django::native::accepts_gzip(ae); },
        nb::arg("accept_encoding"));
  m.def("gzip_content_too_short",
        [](int content_len, int min_len) {
          return django::native::gzip_content_too_short(content_len, min_len);
        },
        nb::arg("content_len"), nb::arg("min_len") = 200);
  m.def("host_needs_www_prefix",
        [](const std::string& host) {
          return django::native::host_needs_www_prefix(host);
        },
        nb::arg("host"));
  m.def("www_redirect_url",
        [](const std::string& scheme, const std::string& host,
           const std::string& path) {
          return django::native::www_redirect_url(scheme, host, path);
        },
        nb::arg("scheme"), nb::arg("host"), nb::arg("path"));
  m.def("xframe_options_value",
        [](const std::string& setting_value) {
          return django::native::xframe_options_value(setting_value);
        },
        nb::arg("setting_value"));

  // --- Fat middleware bodies (one crossing per process_*; chain stays Python)
  m.def(
      "security_process_request",
      [](bool redirect_enabled, bool is_secure, const std::string& path_lstrip,
         const std::string& full_path, const std::string& redirect_host,
         const std::string& request_host, nb::sequence exempt_patterns) {
        std::vector<std::string> pats;
        for (nb::handle h : exempt_patterns) {
          pats.push_back(nb::cast<std::string>(h));
        }
        auto url = django::native::security_process_request(
            redirect_enabled, is_secure, path_lstrip, full_path, redirect_host,
            request_host, pats);
        if (!url) {
          return nb::object(nb::none());
        }
        return nb::object(nb::str(url->c_str(), url->size()));
      },
      nb::arg("redirect_enabled"), nb::arg("is_secure"), nb::arg("path_lstrip"),
      nb::arg("full_path"), nb::arg("redirect_host"), nb::arg("request_host"),
      nb::arg("exempt_patterns"));
  m.def(
      "security_process_response",
      [](bool is_secure, bool has_sts_header, int sts_seconds,
         bool sts_include_subdomains, bool sts_preload,
         bool content_type_nosniff, bool has_content_type_options,
         nb::handle referrer_policy, bool has_referrer_policy,
         nb::handle cross_origin_opener_policy, bool has_coop) {
        return django::native::security_process_response(
            is_secure, has_sts_header, sts_seconds, sts_include_subdomains,
            sts_preload, content_type_nosniff, has_content_type_options,
            referrer_policy, has_referrer_policy, cross_origin_opener_policy,
            has_coop);
      },
      nb::arg("is_secure"), nb::arg("has_sts_header"), nb::arg("sts_seconds"),
      nb::arg("sts_include_subdomains"), nb::arg("sts_preload"),
      nb::arg("content_type_nosniff"), nb::arg("has_content_type_options"),
      nb::arg("referrer_policy").none(), nb::arg("has_referrer_policy"),
      nb::arg("cross_origin_opener_policy").none(), nb::arg("has_coop"));
  m.def(
      "xframe_process_response",
      [](bool already_has_header, bool xframe_options_exempt,
         const std::string& setting_value) -> nb::object {
        auto v = django::native::xframe_process_response(
            already_has_header, xframe_options_exempt, setting_value);
        if (!v) {
          return nb::none();
        }
        return nb::str(v->c_str(), v->size());
      },
      nb::arg("already_has_header"), nb::arg("xframe_options_exempt"),
      nb::arg("setting_value"));
  m.def(
      "common_content_length_header",
      [](bool streaming, bool already_has, std::size_t content_len) -> nb::object {
        auto v = django::native::common_content_length_header(
            streaming, already_has, content_len);
        if (!v) {
          return nb::none();
        }
        return nb::str(v->c_str(), v->size());
      },
      nb::arg("streaming"), nb::arg("already_has_content_length"),
      nb::arg("content_len"));
  m.def(
      "common_www_redirect_url",
      [](bool prepend_www, const std::string& host, const std::string& scheme,
         const std::string& path) -> nb::object {
        auto v = django::native::common_www_redirect_url(prepend_www, host,
                                                         scheme, path);
        if (!v) {
          return nb::none();
        }
        return nb::str(v->c_str(), v->size());
      },
      nb::arg("prepend_www"), nb::arg("host"), nb::arg("scheme"),
      nb::arg("path"));
  m.def(
      "gzip_process_response_plan",
      [](bool streaming, int content_len, int min_len, bool has_content_encoding,
         nb::handle accept_encoding, nb::handle etag) {
        std::string ae =
            accept_encoding.is_none() ? "" : nb::cast<std::string>(nb::str(accept_encoding));
        std::string et =
            etag.is_none() ? "" : nb::cast<std::string>(nb::str(etag));
        return django::native::gzip_process_response_plan(
            streaming, content_len, min_len, has_content_encoding, ae, et);
      },
      nb::arg("streaming"), nb::arg("content_len"), nb::arg("min_len") = 200,
      nb::arg("has_content_encoding"), nb::arg("accept_encoding"),
      nb::arg("etag") = "");
  m.def(
      "conditional_needs_etag",
      [](const std::string& cache_control) {
        return django::native::conditional_needs_etag(cache_control);
      },
      nb::arg("cache_control"));
  m.def(
      "session_cookie_expiry",
      [](bool expire_at_browser_close, int expiry_age_seconds, double now_unix) {
        return django::native::session_cookie_expiry(
            expire_at_browser_close, expiry_age_seconds, now_unix);
      },
      nb::arg("expire_at_browser_close"), nb::arg("expiry_age_seconds"),
      nb::arg("now_unix"));
  m.def(
      "session_process_response_plan",
      [](bool accessed, bool modified, bool empty, bool cookie_in_request,
         bool save_every_request, int status_code, bool expire_at_browser_close,
         int expiry_age_seconds, double now_unix) {
        return django::native::session_process_response_plan(
            accessed, modified, empty, cookie_in_request, save_every_request,
            status_code, expire_at_browser_close, expiry_age_seconds, now_unix);
      },
      nb::arg("accessed"), nb::arg("modified"), nb::arg("empty"),
      nb::arg("cookie_in_request"), nb::arg("save_every_request"),
      nb::arg("status_code"), nb::arg("expire_at_browser_close"),
      nb::arg("expiry_age_seconds"), nb::arg("now_unix"));
  m.def(
      "session_load_key",
      [](nb::handle cookie_value, int min_length) -> nb::object {
        if (cookie_value.is_none()) {
          return nb::none();
        }
        std::string v = nb::cast<std::string>(nb::str(cookie_value));
        auto key = django::native::session_load_key(v, min_length);
        if (!key) {
          return nb::none();
        }
        return nb::str(key->c_str(), key->size());
      },
      nb::arg("cookie_value"), nb::arg("min_length") = 8);
  m.def(
      "csrf_process_view_gate",
      [](bool csrf_processing_done, bool csrf_exempt, const std::string& method,
         bool dont_enforce) {
        return django::native::csrf_process_view_gate(
            csrf_processing_done, csrf_exempt, method, dont_enforce);
      },
      nb::arg("csrf_processing_done"), nb::arg("csrf_exempt"), nb::arg("method"),
      nb::arg("dont_enforce"));
  m.def(
      "csrf_is_safe_method",
      [](const std::string& method) {
        return django::native::csrf_is_safe_method(method);
      },
      nb::arg("method"));
  m.def(
      "csrf_secrets_match",
      [](const std::string& request_token, const std::string& csrf_secret,
         int secret_len, int token_len) {
        return django::native::csrf_secrets_match(request_token, csrf_secret,
                                                  secret_len, token_len);
      },
      nb::arg("request_token"), nb::arg("csrf_secret"),
      nb::arg("secret_len") = 32, nb::arg("token_len") = 64);
  m.def(
      "csrf_origin_verified",
      [](const std::string& request_origin, const std::string& good_origin,
         const std::vector<std::string>& exact_origins,
         const std::vector<std::pair<std::string, std::string>>&
             subdomain_patterns) {
        return django::native::csrf_origin_verified(
            request_origin, good_origin, exact_origins, subdomain_patterns);
      },
      nb::arg("request_origin"), nb::arg("good_origin") = "",
      nb::arg("exact_origins") = std::vector<std::string>{},
      nb::arg("subdomain_patterns") =
          std::vector<std::pair<std::string, std::string>>{});
  m.def(
      "csrf_check_referer",
      [](const std::string& referer_header, const std::string& good_referer,
         const std::vector<std::string>& trusted_hosts) {
        return django::native::csrf_check_referer(referer_header, good_referer,
                                                  trusted_hosts);
      },
      nb::arg("referer_header") = "", nb::arg("good_referer") = "",
      nb::arg("trusted_hosts") = std::vector<std::string>{});
  m.def(
      "auth_login_required_gate",
      [](bool login_required, bool is_authenticated) {
        return django::native::auth_login_required_gate(login_required,
                                                        is_authenticated);
      },
      nb::arg("login_required"), nb::arg("is_authenticated"));
  m.def(
      "is_native_stock_middleware_path",
      [](const std::string& path) {
        return django::native::is_native_stock_middleware_path(path);
      },
      nb::arg("dotted_path"));
  m.def(
      "native_stock_chain_call",
      [](nb::sequence specs, nb::handle request, nb::handle get_response) {
        return django::native::native_stock_chain_call(specs, request,
                                                       get_response);
      },
      nb::arg("specs"), nb::arg("request"), nb::arg("get_response"));
  m.def(
      "hybrid_process_request",
      [](nb::dict cfg, nb::handle request) {
        return django::native::hybrid_process_request(cfg, request);
      },
      nb::arg("cfg"), nb::arg("request"));
  m.def(
      "hybrid_process_response",
      [](nb::dict cfg, nb::handle request, nb::handle response) {
        return django::native::hybrid_process_response(cfg, request, response);
      },
      nb::arg("cfg"), nb::arg("request"), nb::arg("response"));
  m.def(
      "session_response_needs_work",
      [](bool accessed, bool modified, bool save_every_request) {
        return django::native::session_response_needs_work(
            accessed, modified, save_every_request);
      },
      nb::arg("accessed"), nb::arg("modified"),
      nb::arg("save_every_request"));
  m.def("message_tags_join",
        [](const std::string& extra_tags, const std::string& level_tag) {
          return django::native::message_tags_join(extra_tags, level_tag);
        },
        nb::arg("extra_tags"), nb::arg("level_tag"));
  m.def("hashed_static_basename",
        [](const std::string& root, const std::string& hash_with_dot,
           const std::string& ext) {
          return django::native::hashed_static_basename(root, hash_with_dot, ext);
        },
        nb::arg("root"), nb::arg("hash_with_dot"), nb::arg("ext"));
  m.def("posix_path_join",
        [](const std::string& directory, const std::string& basename) {
          return django::native::posix_path_join(directory, basename);
        },
        nb::arg("directory"), nb::arg("basename"));
  m.def("json_use_indent_separators",
        [](bool has_indent) {
          return django::native::json_use_indent_separators(has_indent);
        },
        nb::arg("has_indent"));
  m.def("datetime_iso_utc_z",
        [](const std::string& iso) {
          return django::native::datetime_iso_utc_z(iso);
        },
        nb::arg("iso"));
  m.def("string_has_newlines",
        [](const std::string& s) {
          return django::native::string_has_newlines(s);
        },
        nb::arg("s"));
  m.def(
      "split_email_address",
      [](const std::string& address) {
        auto p = django::native::split_email_address(address);
        return nb::make_tuple(p.ok, p.local, p.domain);
      },
      nb::arg("address"));
  m.def("model_meta_label",
        [](const std::string& app_label, const std::string& object_name) {
          return django::native::model_meta_label(app_label, object_name);
        },
        nb::arg("app_label"), nb::arg("object_name"));
  m.def("manager_str",
        [](const std::string& model_label, const std::string& manager_name) {
          return django::native::manager_str(model_label, manager_name);
        },
        nb::arg("model_label"), nb::arg("manager_name"));
  m.def("from_queryset_class_name",
        [](const std::string& manager_cls, const std::string& qs_cls) {
          return django::native::from_queryset_class_name(manager_cls, qs_cls);
        },
        nb::arg("manager_cls"), nb::arg("qs_cls"));
  m.def("migration_node_key",
        [](const std::string& app_label, const std::string& name) {
          return django::native::migration_node_key(app_label, name);
        },
        nb::arg("app_label"), nb::arg("name"));
  m.def("perm_codename",
        [](const std::string& action, const std::string& model_name) {
          return django::native::perm_codename(action, model_name);
        },
        nb::arg("action"), nb::arg("model_name"));
  m.def("user_can_authenticate",
        [](bool has_is_active, bool is_active) {
          return django::native::user_can_authenticate(has_is_active, is_active);
        },
        nb::arg("has_is_active"), nb::arg("is_active"));
  m.def("signal_has_receivers",
        [](int n) { return django::native::signal_has_receivers(n); },
        nb::arg("n_receivers"));
  m.def(
      "split_dotted_path",
      [](const std::string& dotted) {
        auto p = django::native::split_dotted_path(dotted);
        return nb::make_tuple(p.ok, p.module, p.attr);
      },
      nb::arg("dotted"));
  m.def("app_module_path",
        [](const std::string& app_name, const std::string& submodule) {
          return django::native::app_module_path(app_name, submodule);
        },
        nb::arg("app_name"), nb::arg("submodule"));
  m.def("renamed_method_warning",
        [](const std::string& class_name, const std::string& old_name,
           const std::string& new_name) {
          return django::native::renamed_method_warning(class_name, old_name,
                                                        new_name);
        },
        nb::arg("class_name"), nb::arg("old_name"), nb::arg("new_name"));
  m.def("path_ends_with_py",
        [](const std::string& path) {
          return django::native::path_ends_with_py(path);
        },
        nb::arg("path"));
  m.def(
      "path_has_any_suffix",
      [](const std::string& path, const std::vector<std::string>& suffixes) {
        return django::native::path_has_any_suffix(path, suffixes);
      },
      nb::arg("path"), nb::arg("suffixes"));
  m.def("postgres_arrayfield_path_shorten",
        [](const std::string& path) {
          return django::native::postgres_arrayfield_path_shorten(path);
        },
        nb::arg("path"));
  m.def("filename_needs_quotes",
        [](const std::string& filename) {
          return django::native::filename_needs_quotes(filename);
        },
        nb::arg("filename"));

  // --- menu 1-12 -----------------------------------------------------------
  m.def("paginator_num_pages",
        [](int count, int per_page, int orphans, bool allow_empty) {
          return django::native::paginator_num_pages(count, per_page, orphans,
                                                     allow_empty);
        },
        nb::arg("count"), nb::arg("per_page"), nb::arg("orphans"),
        nb::arg("allow_empty_first_page"));
  m.def("paginator_page_bottom",
        [](int number, int per_page) {
          return django::native::paginator_page_bottom(number, per_page);
        },
        nb::arg("number"), nb::arg("per_page"));
  m.def("paginator_page_top",
        [](int number, int per_page, int orphans, int count) {
          return django::native::paginator_page_top(number, per_page, orphans,
                                                    count);
        },
        nb::arg("number"), nb::arg("per_page"), nb::arg("orphans"),
        nb::arg("count"));
  m.def("paginator_number_range_code",
        [](int number, int num_pages) {
          return django::native::paginator_number_range_code(number, num_pages);
        },
        nb::arg("number"), nb::arg("num_pages"));
  m.def("url_is_relative_path",
        [](const std::string& to) {
          return django::native::url_is_relative_path(to);
        },
        nb::arg("to"));
  m.def("url_feels_like_url",
        [](const std::string& to) {
          return django::native::url_feels_like_url(to);
        },
        nb::arg("to"));
  m.def("formset_total_forms_bound",
        [](int submitted, int absolute_max) {
          return django::native::formset_total_forms_bound(submitted,
                                                           absolute_max);
        },
        nb::arg("submitted"), nb::arg("absolute_max"));
  m.def("formset_total_forms_unbound",
        [](int initial_forms, int min_num, int extra, int max_num) {
          return django::native::formset_total_forms_unbound(
              initial_forms, min_num, extra, max_num);
        },
        nb::arg("initial_forms"), nb::arg("min_num"), nb::arg("extra"),
        nb::arg("max_num"));
  m.def("path_has_dotdot",
        [](const std::string& path) {
          return django::native::path_has_dotdot(path);
        },
        nb::arg("path"));
  m.def("storage_normalize_name",
        [](const std::string& name) {
          return django::native::storage_normalize_name(name);
        },
        nb::arg("name"));
  m.def("storage_alternative_name",
        [](const std::string& root, const std::string& random7,
           const std::string& ext) {
          return django::native::storage_alternative_name(root, random7, ext);
        },
        nb::arg("root"), nb::arg("random7"), nb::arg("ext"));
  m.def("storage_name_available",
        [](bool exists, bool has_max_length, int name_len, int max_length) {
          return django::native::storage_name_available(
              exists, has_max_length, name_len, max_length);
        },
        nb::arg("exists"), nb::arg("has_max_length"), nb::arg("name_len"),
        nb::arg("max_length") = 0);
  m.def("middleware_capability_ok",
        [](bool sync_capable, bool async_capable) {
          return django::native::middleware_capability_ok(sync_capable,
                                                          async_capable);
        },
        nb::arg("sync_capable"), nb::arg("async_capable"));
  m.def("sitemap_priority_valid",
        [](double priority) {
          return django::native::sitemap_priority_valid(priority);
        },
        nb::arg("priority"));
  m.def("sitemap_changefreq_valid",
        [](const std::string& freq) {
          return django::native::sitemap_changefreq_valid(freq);
        },
        nb::arg("freq"));
  m.def("ordinal_suffix_kind",
        [](int value) { return django::native::ordinal_suffix_kind(value); },
        nb::arg("value"));
  m.def("intcomma_ascii",
        [](const std::string& digits) {
          return django::native::intcomma_ascii(digits);
        },
        nb::arg("digits"));
  m.def("check_is_serious",
        [](int level, int threshold) {
          return django::native::check_is_serious(level, threshold);
        },
        nb::arg("level"), nb::arg("threshold"));
  m.def("path_with_query",
        [](const std::string& path, const std::string& query) {
          return django::native::path_with_query(path, query);
        },
        nb::arg("path"), nb::arg("query"));
  m.def("ensure_leading_slash",
        [](const std::string& path) {
          return django::native::ensure_leading_slash(path);
        },
        nb::arg("path"));
  m.def("redirect_paths_equal",
        [](const std::string& a, const std::string& b) {
          return django::native::redirect_paths_equal(a, b);
        },
        nb::arg("a"), nb::arg("b"));
  m.def("wkt_point",
        [](const std::string& x, const std::string& y) {
          return django::native::wkt_point(x, y);
        },
        nb::arg("x"), nb::arg("y"));
  m.def("postgres_empty_array_literal",
        []() { return django::native::postgres_empty_array_literal(); });

  // --- menu deep: generic views → test utils --------------------------------
  m.def("list_context_object_name",
        [](const std::string& model_name) {
          return django::native::list_context_object_name(model_name);
        },
        nb::arg("model_name"));
  m.def(
      "http_method_in_names",
      [](const std::string& method_lower,
         const std::vector<std::string>& names) {
        return django::native::http_method_in_names(method_lower, names);
      },
      nb::arg("method_lower"), nb::arg("names"));
  m.def("page_token_is_last",
        [](const std::string& page) {
          return django::native::page_token_is_last(page);
        },
        nb::arg("page"));
  m.def("model_template_name",
        [](const std::string& app_label, const std::string& object_name,
           const std::string& suffix) {
          return django::native::model_template_name(app_label, object_name,
                                                     suffix);
        },
        nb::arg("app_label"), nb::arg("object_name"), nb::arg("suffix"));
  m.def("modelform_class_name",
        [](const std::string& model_name) {
          return django::native::modelform_class_name(model_name);
        },
        nb::arg("model_name"));
  m.def(
      "form_field_included",
      [](bool editable, bool fields_is_none, bool in_fields, bool exclude_active,
         bool in_exclude) {
        return django::native::form_field_included(
            editable, fields_is_none, in_fields, exclude_active, in_exclude);
      },
      nb::arg("editable"), nb::arg("fields_is_none"), nb::arg("in_fields"),
      nb::arg("exclude_active"), nb::arg("in_exclude"));
  m.def("admin_quote",
        [](const std::string& s) { return django::native::admin_quote(s); },
        nb::arg("s"));
  m.def("lookup_key_endswith",
        [](const std::string& key, const std::string& suffix) {
          return django::native::lookup_key_endswith(key, suffix);
        },
        nb::arg("key"), nb::arg("suffix"));
  m.def("prepare_lookup_isnull",
        [](const std::string& value_lower) {
          return django::native::prepare_lookup_isnull(value_lower);
        },
        nb::arg("value_lower"));
  m.def("paths_equal",
        [](const std::string& a, const std::string& b) {
          return django::native::paths_equal(a, b);
        },
        nb::arg("a"), nb::arg("b"));
  m.def("strings_ci_equal_ascii",
        [](const std::string& a, const std::string& b) {
          return django::native::strings_ci_equal_ascii(a, b);
        },
        nb::arg("a"), nb::arg("b"));
  m.def("migration_filename",
        [](const std::string& name) {
          return django::native::migration_filename(name);
        },
        nb::arg("name"));
  m.def("introspection_is_table",
        [](const std::string& type_code) {
          return django::native::introspection_is_table(type_code);
        },
        nb::arg("type_code"));
  m.def("combined_expression_sql",
        [](const std::string& lhs, const std::string& connector,
           const std::string& rhs) {
          return django::native::combined_expression_sql(lhs, connector, rhs);
        },
        nb::arg("lhs"), nb::arg("connector"), nb::arg("rhs"));
  m.def("sql_cast_as_numeric",
        [](const std::string& sql) {
          return django::native::sql_cast_as_numeric(sql);
        },
        nb::arg("sql"));
  m.def("cache_timestamp_expired",
        [](bool exp_is_none, double exp, double now) {
          return django::native::cache_timestamp_expired(exp_is_none, exp, now);
        },
        nb::arg("exp_is_none"), nb::arg("exp"), nb::arg("now"));
  m.def("cache_file_name",
        [](const std::string& hexdigest, const std::string& suffix) {
          return django::native::cache_file_name(hexdigest, suffix);
        },
        nb::arg("hexdigest"), nb::arg("suffix"));
  m.def("cache_cull_needed",
        [](int num_entries, int max_entries) {
          return django::native::cache_cull_needed(num_entries, max_entries);
        },
        nb::arg("num_entries"), nb::arg("max_entries"));
  m.def("cache_cull_sample_size",
        [](int num_entries, int cull_frequency) {
          return django::native::cache_cull_sample_size(num_entries,
                                                        cull_frequency);
        },
        nb::arg("num_entries"), nb::arg("cull_frequency"));
  m.def("wsgi_request_path",
        [](const std::string& script_name, const std::string& path_info) {
          return django::native::wsgi_request_path(script_name, path_info);
        },
        nb::arg("script_name"), nb::arg("path_info"));
  m.def("exception_status_code",
        [](const std::string& kind) {
          return django::native::exception_status_code(kind);
        },
        nb::arg("kind"));
  m.def("postgres_normalize_spaces",
        [](const std::string& val) {
          return django::native::postgres_normalize_spaces(val);
        },
        nb::arg("val"));
  m.def("postgres_psql_escape",
        [](const std::string& query) {
          return django::native::postgres_psql_escape(query);
        },
        nb::arg("query"));
  m.def("search_vector_match_sql",
        [](const std::string& lhs, const std::string& rhs) {
          return django::native::search_vector_match_sql(lhs, rhs);
        },
        nb::arg("lhs"), nb::arg("rhs"));
  m.def("feed_protocol",
        [](bool secure) { return django::native::feed_protocol(secure); },
        nb::arg("secure"));
  m.def("feed_url_is_network_path",
        [](const std::string& url) {
          return django::native::feed_url_is_network_path(url);
        },
        nb::arg("url"));
  m.def("feed_url_has_scheme",
        [](const std::string& url) {
          return django::native::feed_url_has_scheme(url);
        },
        nb::arg("url"));
  m.def("feed_network_path_url",
        [](const std::string& protocol, const std::string& url) {
          return django::native::feed_network_path_url(protocol, url);
        },
        nb::arg("protocol"), nb::arg("url"));
  m.def("feed_absolute_url",
        [](const std::string& protocol, const std::string& domain,
           const std::string& url) {
          return django::native::feed_absolute_url(protocol, domain, url);
        },
        nb::arg("protocol"), nb::arg("domain"), nb::arg("url"));
  m.def("dotted_qualname",
        [](const std::string& module, const std::string& qualname) {
          return django::native::dotted_qualname(module, qualname);
        },
        nb::arg("module"), nb::arg("qualname"));
  m.def("strip_p_tags",
        [](const std::string& value) {
          return django::native::strip_p_tags(value);
        },
        nb::arg("value"));
  m.def("approximate_equal",
        [](double val, double other, int places) {
          return django::native::approximate_equal(val, other, places);
        },
        nb::arg("val"), nb::arg("other"), nb::arg("places"));
  m.def(
      "http_allow_header",
      [](const std::vector<std::string>& methods) {
        return django::native::http_allow_header(methods);
      },
      nb::arg("methods"));
  m.def("ensure_trailing_slash",
        [](const std::string& url) {
          return django::native::ensure_trailing_slash(url);
        },
        nb::arg("url"));
  m.def("ascii_lower",
        [](const std::string& s) {
          return django::native::string_ascii_lower(s);
        },
        nb::arg("s"));
  m.def("management_command_name",
        [](const std::string& path) {
          return django::native::management_command_name(path);
        },
        nb::arg("path"));
  m.def("asgi_path_info",
        [](const std::string& path, const std::string& script_name) {
          return django::native::asgi_path_info(path, script_name);
        },
        nb::arg("path"), nb::arg("script_name"));

  // --- menu 1-12 unit-testable ----------------------------------------------
  m.def("field_str",
        [](const std::string& model_label, const std::string& name) {
          return django::native::field_str(model_label, name);
        },
        nb::arg("model_label"), nb::arg("name"));
  m.def("field_repr",
        [](const std::string& path, bool has_name, const std::string& name) {
          return django::native::field_repr(path, has_name, name);
        },
        nb::arg("path"), nb::arg("has_name"), nb::arg("name") = "");
  m.def("verbose_name_from_name",
        [](const std::string& name) {
          return django::native::verbose_name_from_name(name);
        },
        nb::arg("name"));
  m.def("field_name_check_code",
        [](const std::string& name) {
          return django::native::field_name_check_code(name);
        },
        nb::arg("name"));
  m.def("field_column_name",
        [](const std::string& attname, const std::string& db_column) {
          return django::native::field_column_name(attname, db_column);
        },
        nb::arg("attname"), nb::arg("db_column") = "");
  m.def("aggregate_default_alias",
        [](const std::string& expr_name, const std::string& agg_name) {
          return django::native::aggregate_default_alias(expr_name, agg_name);
        },
        nb::arg("expr_name"), nb::arg("agg_name"));
  m.def("sql_distinct_prefix",
        [](bool distinct) {
          return django::native::sql_distinct_prefix(distinct);
        },
        nb::arg("distinct"));
  m.def("index_column_with_order",
        [](const std::string& column, bool descending) {
          return django::native::index_column_with_order(column, descending);
        },
        nb::arg("column"), nb::arg("descending"));
  m.def("index_name_fix_leading",
        [](const std::string& name) {
          return django::native::index_name_fix_leading(name);
        },
        nb::arg("name"));
  m.def("admin_can_show_all",
        [](int result_count, int list_max_show_all) {
          return django::native::admin_can_show_all(result_count,
                                                    list_max_show_all);
        },
        nb::arg("result_count"), nb::arg("list_max_show_all"));
  m.def("admin_is_multi_page",
        [](int result_count, int list_per_page) {
          return django::native::admin_is_multi_page(result_count, list_per_page);
        },
        nb::arg("result_count"), nb::arg("list_per_page"));
  m.def("query_string_with_prefix",
        [](const std::string& encoded) {
          return django::native::query_string_with_prefix(encoded);
        },
        nb::arg("encoded"));
  m.def(
      "css_classes_join",
      [](const std::vector<std::string>& classes) {
        return django::native::css_classes_join(classes);
      },
      nb::arg("classes"));
  m.def("password_reset_token_join",
        [](const std::string& ts_b36, const std::string& hash_hex) {
          return django::native::password_reset_token_join(ts_b36, hash_hex);
        },
        nb::arg("ts_b36"), nb::arg("hash_hex"));
  m.def(
      "password_reset_token_split",
      [](const std::string& token) {
        auto r = django::native::password_reset_token_split(token);
        return nb::make_tuple(r.ok, r.ts_b36, r.rest);
      },
      nb::arg("token"));
  m.def("password_meets_min_length",
        [](int password_len, int min_length) {
          return django::native::password_meets_min_length(password_len,
                                                           min_length);
        },
        nb::arg("password_len"), nb::arg("min_length"));
  m.def("password_is_numeric_only",
        [](const std::string& password) {
          return django::native::password_is_numeric_only(password);
        },
        nb::arg("password"));
  m.def("migration_node_repr",
        [](const std::string& cls, const std::string& app,
           const std::string& name) {
          return django::native::migration_node_repr(cls, app, name);
        },
        nb::arg("cls"), nb::arg("app"), nb::arg("name"));
  m.def("serializer_datetime_import",
        []() { return django::native::serializer_datetime_import(); });
  m.def("sitemap_absolute_url",
        [](const std::string& protocol, const std::string& domain,
           const std::string& path) {
          return django::native::sitemap_absolute_url(protocol, domain, path);
        },
        nb::arg("protocol"), nb::arg("domain"), nb::arg("path"));
  m.def("sitemap_paged_url",
        [](const std::string& absolute_url, int page) {
          return django::native::sitemap_paged_url(absolute_url, page);
        },
        nb::arg("absolute_url"), nb::arg("page"));
  m.def("x_robots_tag_value",
        []() { return django::native::x_robots_tag_value(); });
  m.def("http_status_session_saveable",
        [](int status_code) {
          return django::native::http_status_session_saveable(status_code);
        },
        nb::arg("status_code"));
  m.def("resource_was_modified",
        [](bool header_missing, double mtime, double header_mtime) {
          return django::native::resource_was_modified(header_missing, mtime,
                                                       header_mtime);
        },
        nb::arg("header_missing"), nb::arg("mtime"), nb::arg("header_mtime"));
  m.def("template_register_name",
        [](const std::string& explicit_name, const std::string& func_name) {
          return django::native::template_register_name(explicit_name,
                                                        func_name);
        },
        nb::arg("explicit_name"), nb::arg("func_name"));
  m.def("normalize_ascii_whitespace",
        [](const std::string& s) {
          return django::native::normalize_ascii_whitespace(s);
        },
        nb::arg("s"));
  m.def("html_boolean_attr_is_true",
        [](const std::string& name, const std::string& value) {
          return django::native::html_boolean_attr_is_true(name, value);
        },
        nb::arg("name"), nb::arg("value"));
  m.def("sql_func_call",
        [](const std::string& function, const std::string& expressions) {
          return django::native::sql_func_call(function, expressions);
        },
        nb::arg("function"), nb::arg("expressions"));
  m.def("field_display_method_name",
        [](const std::string& field_name) {
          return django::native::field_display_method_name(field_name);
        },
        nb::arg("field_name"));
  m.def("optimizer_lists_equal_len",
        [](int a, int b) {
          return django::native::optimizer_lists_equal_len(a, b);
        },
        nb::arg("a"), nb::arg("b"));

  // --- Tier A/B ------------------------------------------------------------
  m.def("related_name_ends_plus",
        [](const std::string& name) {
          return django::native::related_name_ends_plus(name);
        },
        nb::arg("name"));
  m.def("related_name_is_identifier",
        [](const std::string& name) {
          return django::native::related_name_is_identifier(name);
        },
        nb::arg("name"));
  m.def("related_query_name_ends_underscore",
        [](const std::string& name) {
          return django::native::related_query_name_ends_underscore(name);
        },
        nb::arg("name"));
  m.def("related_query_name_has_lookup_sep",
        [](const std::string& name) {
          return django::native::related_query_name_has_lookup_sep(name);
        },
        nb::arg("name"));
  m.def("fk_default_name",
        [](const std::string& model_name, const std::string& pk_name) {
          return django::native::fk_default_name(model_name, pk_name);
        },
        nb::arg("model_name"), nb::arg("pk_name"));
  m.def("related_filter_key",
        [](const std::string& field_name, const std::string& rh_field) {
          return django::native::related_filter_key(field_name, rh_field);
        },
        nb::arg("field_name"), nb::arg("rh_field"));
  m.def("constraint_deconstruct_path",
        [](const std::string& path) {
          return django::native::constraint_deconstruct_path(path);
        },
        nb::arg("path"));
  m.def("sql_varchar_type",
        [](bool has_max_length, int max_length) {
          return django::native::sql_varchar_type(has_max_length, max_length);
        },
        nb::arg("has_max_length"), nb::arg("max_length") = 0);
  m.def("sql_decimal_type",
        [](int max_digits, int decimal_places) {
          return django::native::sql_decimal_type(max_digits, decimal_places);
        },
        nb::arg("max_digits"), nb::arg("decimal_places"));
  m.def("admin_selectfilter_class",
        [](bool is_stacked) {
          return django::native::admin_selectfilter_class(is_stacked);
        },
        nb::arg("is_stacked"));
  m.def("admin_site_repr",
        [](const std::string& cls, const std::string& name) {
          return django::native::admin_site_repr(cls, name);
        },
        nb::arg("cls"), nb::arg("name"));
  m.def("permission_str",
        [](const std::string& content_type, const std::string& name) {
          return django::native::permission_str(content_type, name);
        },
        nb::arg("content_type"), nb::arg("name"));
  m.def("admin_facet_count_key",
        [](int index) {
          return django::native::admin_facet_count_key(index);
        },
        nb::arg("index"));
  m.def("extract_lookup_name",
        [](const std::string& lookup) {
          return django::native::extract_lookup_name(lookup);
        },
        nb::arg("lookup"));
  m.def("sql_now_sqlite",
        []() { return django::native::sql_now_sqlite(); });
  m.def("sql_now_postgresql",
        []() { return django::native::sql_now_postgresql(); });
  m.def("feed_tag_uri",
        [](const std::string& hostname, const std::string& date_suffix,
           const std::string& path, const std::string& fragment) {
          return django::native::feed_tag_uri(hostname, date_suffix, path,
                                              fragment);
        },
        nb::arg("hostname"), nb::arg("date_suffix"), nb::arg("path"),
        nb::arg("fragment"));
  m.def("progress_percent",
        [](int count, int total) {
          return django::native::progress_percent(count, total);
        },
        nb::arg("count"), nb::arg("total"));
  m.def("progress_done_width",
        [](int percent, int width) {
          return django::native::progress_done_width(percent, width);
        },
        nb::arg("percent"), nb::arg("width"));
  m.def("backend_vendor_is",
        [](const std::string& vendor, const std::string& expected) {
          return django::native::backend_vendor_is(vendor, expected);
        },
        nb::arg("vendor"), nb::arg("expected"));
  m.def("management_prog",
        [](const std::string& basename, const std::string& subcommand) {
          return django::native::management_prog(basename, subcommand);
        },
        nb::arg("basename"), nb::arg("subcommand"));
  m.def("filefield_default_max_length",
        []() { return django::native::filefield_default_max_length(); });
  m.def("jsonfield_internal_type",
        []() { return django::native::jsonfield_internal_type(); });
  m.def("test_label_looks_like_path",
        [](const std::string& label) {
          return django::native::test_label_looks_like_path(label);
        },
        nb::arg("label"));
  m.def("debug_template_path",
        [](const std::string& name) {
          return django::native::debug_template_path(name);
        },
        nb::arg("name"));
  m.def("date_year_in_range",
        [](int year) { return django::native::date_year_in_range(year); },
        nb::arg("year"));
  m.def("unique_constraint_name",
        [](const std::string& model, const std::string& fields_joined) {
          return django::native::unique_constraint_name(model, fields_joined);
        },
        nb::arg("model"), nb::arg("fields_joined"));
  m.def("db_host_is_unix_socket",
        [](const std::string& host) {
          return django::native::db_host_is_unix_socket(host);
        },
        nb::arg("host"));
  m.def("postgres_set_timezone_sql",
        []() { return django::native::postgres_set_timezone_sql(); });
  m.def("mysql_isolation_level_valid",
        [](const std::string& level) {
          return django::native::mysql_isolation_level_valid(level);
        },
        nb::arg("level"));
  m.def(
      "simple_select_eq_limit_sql",
      [](const std::string& quoted_table,
         const std::vector<std::string>& quoted_cols,
         const std::string& quoted_where_col, int limit) {
        return django::native::simple_select_eq_limit_sql(
            quoted_table, quoted_cols, quoted_where_col, limit);
      },
      nb::arg("quoted_table"), nb::arg("quoted_cols"),
      nb::arg("quoted_where_col"), nb::arg("limit"));
  m.def(
      "simple_select_all_sql",
      [](const std::string& quoted_table,
         const std::vector<std::string>& quoted_cols, int limit) {
        return django::native::simple_select_all_sql(quoted_table, quoted_cols,
                                                     limit);
      },
      nb::arg("quoted_table"), nb::arg("quoted_cols"), nb::arg("limit") = 0);
  m.def(
      "simple_select_in_sql",
      [](const std::string& quoted_table,
         const std::vector<std::string>& quoted_cols,
         const std::string& quoted_where_col, int n_placeholders) {
        return django::native::simple_select_in_sql(
            quoted_table, quoted_cols, quoted_where_col, n_placeholders);
      },
      nb::arg("quoted_table"), nb::arg("quoted_cols"),
      nb::arg("quoted_where_col"), nb::arg("n_placeholders"));
  m.def(
      "simple_update_eq_sql",
      [](const std::string& quoted_table,
         const std::vector<std::string>& quoted_set_cols,
         const std::string& quoted_where_col) {
        return django::native::simple_update_eq_sql(
            quoted_table, quoted_set_cols, quoted_where_col);
      },
      nb::arg("quoted_table"), nb::arg("quoted_set_cols"),
      nb::arg("quoted_where_col"));
  // --- WSGI handler (C++ owns the request loop; views stay Python) ----------
  m.def(
      "wsgi_handler_lean_eligible",
      [](nb::handle handler) {
        return django::native::wsgi_handler_lean_eligible(handler);
      },
      nb::arg("handler"),
      "True when empty middleware hooks and no ATOMIC_REQUESTS.");
  m.def(
      "wsgi_handler_call",
      [](nb::handle handler, nb::handle environ, nb::handle start_response) {
        return django::native::wsgi_handler_call(handler, environ,
                                                 start_response);
      },
      nb::arg("handler"), nb::arg("environ"), nb::arg("start_response"),
      "Native lean WSGI loop: slim request, exact routes, view, "
      "start_response packing.");
  m.def(
      "wsgi_request_try_lean_init",
      [](nb::handle request, nb::handle environ) {
        return django::native::wsgi_request_try_lean_init(request, environ);
      },
      nb::arg("request"), nb::arg("environ"),
      "Populate WSGIRequest for GET/HEAD empty body; True if applied.");
  m.def(
      "wsgi_lean_get_response",
      [](nb::handle handler, nb::handle request) {
        return django::native::wsgi_lean_get_response(handler, request);
      },
      nb::arg("handler"), nb::arg("request"),
      "Lean get_response: exact-route table or resolve + Python view.");
  m.def(
      "wsgi_environ_is_lean_get",
      [](nb::handle environ) {
        return django::native::wsgi_environ_is_lean_get(environ);
      },
      nb::arg("environ"),
      "True when environ is GET/HEAD with empty body.");
  m.def(
      "render_fortune_page",
      [](const std::vector<std::pair<std::int64_t, std::string>>& rows) {
        return django::native::render_fortune_page(rows);
      },
      nb::arg("rows"));

  // ORM data plane (QuerySet + schema + compiler) — see docs/design/
  extern void register_orm_engine(nb::module_&);
  register_orm_engine(m);

  m.attr("AVAILABLE") = true;
}
