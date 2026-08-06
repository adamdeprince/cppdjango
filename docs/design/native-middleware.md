# Native Middleware Bodies

**Status:** Implemented (dual-path stock middleware + optional pure-C++ chain)  
**Date:** 2026-08-06  

## Design rule (crossing budget)

- **Chain iteration stays in Python** for hybrid / custom stacks. Walking a
  list of callables is cheap.
- **Do not** run the chain in C++ while each link is a Python object: that is
  `C++ → Python → C++` when the body is native.
- **Port middleware *bodies*** to C++. Each `process_request` /
  `process_response` / fat `process_view` makes **at most one** native call.

```text
Python MiddlewareMixin / handler chain
    for mw in chain:                    # Python (cheap)
        mw.process_*(...)               # Python thin shell
            → native.fat_process_*(…)   # one Py→C++ hop
```

## Stock middleware with fat C++ bodies

| Middleware | Native entry |
|------------|--------------|
| `SecurityMiddleware` | `security_process_request`, `security_process_response` |
| `XFrameOptionsMiddleware` | `xframe_process_response` / `xframe_options_value` |
| `CommonMiddleware` | `common_www_redirect_url`, `common_content_length_header` |
| `GZipMiddleware` | `gzip_process_response_plan` |
| `ConditionalGetMiddleware` | `conditional_needs_etag` |
| `SessionMiddleware` | `session_load_key`, `session_process_response_plan`, `session_cookie_expiry` |
| `CsrfViewMiddleware` | `csrf_process_view_gate`, `csrf_secrets_match` (+ existing token helpers) |
| `AuthenticationMiddleware` | marked `native_capable` (user load stays Python / lazy) |
| `LoginRequiredMiddleware` | `auth_login_required_gate` |

User/custom middleware remains pure Python.

## Dual-path

`DJANGO_NATIVE=0` or extension missing → pure Python methods (same behavior).

## Optional pure-C++ stock chain

When **every** `MIDDLEWARE` entry is fully handled by `native_stock_chain_call`
(Security, XFrame, Common only) **and** Common’s Python-only features are off
(`APPEND_SLASH=False`, `PREPEND_WWW=False`, empty `DISALLOWED_USER_AGENTS`),
`BaseHandler.load_middleware` installs a single callable:

```text
native_stock_chain_call(specs, request, get_response)
  → process_request forward (security SSL redirect)
  → get_response (views; process_view hooks empty for this stack)
  → process_response reverse (security headers, xframe, content-length)
```

Any stack that includes Session, CSRF, Auth, GZip, ConditionalGet, or custom
middleware keeps the Python walk + dual-path bodies.

`is_native_stock_middleware_path(path)` reports known dual-path stock classes
(introspection / eligibility helpers).

## Non-goals

- C++ chain runner over **Python** callables (wrong crossing pattern)
- Full SessionStore / Auth user model loading in C++
- CSRF origin/referer verification in C++ (still Python after the gate)
- GZip zlib body in C++ (plan only; compress stays Python)
