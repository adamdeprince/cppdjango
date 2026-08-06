# Native Middleware Bodies

**Status:** Implemented (dual-path stock middleware)  
**Date:** 2026-08-06  

## Design rule (crossing budget)

- **Chain iteration stays in Python.** Walking a list of callables is cheap.
- **Do not** run the chain in C++ while each link is a Python object: that is
  `C++ → Python → C++` when the body is native.
- **Port middleware *bodies*** to C++. Each `process_request` /
  `process_response` makes **at most one** native call.

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
| `XFrameOptionsMiddleware` | `xframe_process_response` |
| `CommonMiddleware` | `common_www_redirect_url`, `common_content_length_header` |
| `GZipMiddleware` | `gzip_process_response_plan` |
| `ConditionalGetMiddleware` | `conditional_needs_etag` |
| `SessionMiddleware` | `session_cookie_expiry` (+ existing status helper) |

User/custom middleware remains pure Python.

## Dual-path

`DJANGO_NATIVE=0` or extension missing → pure Python methods (same behavior).

## Non-goals (this slice)

- C++ chain runner over Python callables
- Full SessionStore / Auth user loading in C++
- CSRF process_view full port (helpers already native; fat view later)
