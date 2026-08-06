# Native WSGI Handler

**Status:** Implemented (dual-path lean loop: slim request + exact routes + lean get_response)  
**Date:** 2026-08-06  

## Thesis

The **WSGI request loop** belongs in C++. **Views stay Python** — that is the
stable, public Django app API. Other planes (ORM data plane, templates, forms,
auth) may also move to C++ independently.

```text
gunicorn / uWSGI
    │  application(environ, start_response)
    ▼
┌──────────────────────────────────────────────────────────┐
│  C++ WSGI handler (lean path)                            │
│  script prefix • request_started                         │
│  slim WSGIRequest (GET/HEAD empty body)                  │
│  exact-route table OR resolve_request                    │
│  Python view → HttpResponse                              │
│  start_response pack • return body                       │
└──────────────────────────────────────────────────────────┘
```

## Dual-path

| Condition | Path |
|-----------|------|
| `DJANGO_NATIVE=0` or extension missing | Pure Python `_python_call` |
| Middleware hooks non-empty or `ATOMIC_REQUESTS` | Pure Python `_python_call` |
| Empty middleware, no atomic, native available | `django.native.wsgi_handler_call` (C++) |

## Load-time vs request-time

| When | What |
|------|------|
| `load_middleware` | `_exact_routes` table for converter-free root `path()` routes |
| Per request (C++) | slim request init, table lookup / resolve, view call, pack |

`settings.MIDDLEWARE` is **not** re-scanned per request for lean eligibility
(hooks emptiness was fixed at load).

## API surface

- `wsgi_handler_lean_eligible(handler) -> bool`
- `wsgi_handler_call(handler, environ, start_response) -> response`
- `wsgi_request_try_lean_init(request, environ) -> bool`
- `wsgi_lean_get_response(handler, request) -> response`
- `wsgi_environ_is_lean_get(environ) -> bool`

## Slim request (1)

For **GET/HEAD** with empty body (no `CONTENT_LENGTH` / zero, no transfer
encoding), ASCII `SCRIPT_NAME`/`PATH_INFO`, no `FORCE_SCRIPT_NAME` / rewrite
URLs:

- Skip `parse_header_parameters` / charset
- Use `BytesIO(b"")` instead of `LimitedStream(wsgi.input)`
- Path join via existing `wsgi_request_path`

Otherwise full Python `WSGIRequest.__init__`.

## Exact-route table (2)

At `load_middleware` when hooks empty, build:

```text
path_info → (callback, args, kwargs, url_name, route)
```

Only root-level `path()` routes **without converters**. Nested `include()` and
dynamic routes fall through to `resolve_request`.

## Lean get_response (3)

C++ `wsgi_lean_get_response`:

1. `_ensure_root_urlconf`
2. `_exact_routes[path_info]` or `resolve_request`
3. call view (Python)
4. `check_response` / optional `render`
5. append `request.close`
6. log 4xx

Exceptions → `response_for_exception` (same as `convert_exception_to_response`).

## Non-goals (this slice)

- Replacing Python views
- Full C++ `WSGIRequest` for POST/multipart
- Middleware stack in this loop (hybrid stacks use Python walk)

## Floor hot-path optimizations

1. **Process-lifetime `LeanCache`**: interned attr names, held callables
   (`set_script_prefix`, `BytesIO`, …), empty tuple/dict — avoid re-import and
   re-intern per request.
2. **Exact-route hit skips `ResolverMatch`**: leave `request.resolver_match`
   as `None` (floor views do not use it).
3. **`request_started`**: read `Signal.receivers` length; skip `send` when 0.
4. **Urlconf pin at load**: `_lean_urlconf_pinned` so lean get_response skips
   `_ensure_root_urlconf` per request.
5. **CallOneArg-style view call** for empty args/kwargs; hardcoded `"200 OK"`;
   skip cookie/header work when empty; skip `check_response` when response is
   non-None.

## Future

- Exact routes for one level of static `include()` prefixes
- North-star: ORM/templates/forms/auth data planes in C++
