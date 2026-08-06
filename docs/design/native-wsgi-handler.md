# Native WSGI Handler

**Status:** Implemented (dual-path lean loop)  
**Date:** 2026-08-06  

## Thesis

The **WSGI request loop** belongs in C++. **Views stay Python** — that is the
stable, public Django app API. Other planes (ORM data plane, templates, forms,
auth) may also move to C++ independently; they are not required for the handler
to own the per-request orchestration.

```text
gunicorn / uWSGI
    │  application(environ, start_response)
    ▼
┌─────────────────────────────────────────────┐
│  C++ WSGI handler (lean path)               │
│  script prefix • signals • request build    │
│  get_response → resolve + **Python view**   │
│  start_response pack • return body          │
└─────────────────────────────────────────────┘
    │
    ▼
  HttpResponse (Python object; body may be bytes)
```

## Dual-path

| Condition | Path |
|-----------|------|
| `DJANGO_NATIVE=0` or extension missing | Pure Python `_python_call` |
| Middleware hooks non-empty or `ATOMIC_REQUESTS` | Pure Python `_python_call` |
| Empty middleware, no atomic, native available | `django.native.wsgi_handler_call` (C++) |

## API surface

- `django.native.wsgi_handler_lean_eligible(handler) -> bool`
- `django.native.wsgi_handler_call(handler, environ, start_response) -> response`

`get_wsgi_application()` still returns `WSGIHandler()`; behavior is inside
`WSGIHandler.__call__`.

## Non-goals (this slice)

- Porting middleware stack to C++
- Replacing Python views
- Full C++ `WSGIRequest` (still constructed via `request_class` in Python)
- ORM / template / forms / auth ports (separate campaigns)

## Future

1. Slim C++ request for GET + empty body (skip unused WSGIRequest work).
2. Exact-route table for static TE paths.
3. Deeper get_response lean path in C++ (resolve + call view) with fewer
   Python attribute hops.
4. North-star: ORM/templates/forms/auth data planes in C++ while views remain
   the Python composition layer.
