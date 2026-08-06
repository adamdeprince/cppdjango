import asyncio
import logging
import types

from asgiref.sync import async_to_sync, iscoroutinefunction, sync_to_async

from django.conf import settings
from django.core.exceptions import ImproperlyConfigured, MiddlewareNotUsed
from django.core.signals import request_finished
from django.db import connections, transaction
from django.urls import get_resolver, set_urlconf
from django.utils.log import log_response
from django.utils.module_loading import import_string

from .exception import convert_exception_to_response

logger = logging.getLogger("django.request")


class BaseHandler:
    _view_middleware = None
    _template_response_middleware = None
    _exception_middleware = None
    _middleware_chain = None
    # Set in load_middleware: no view/template/exception middleware hooks.
    _middleware_hooks_empty = False
    # Pure-C++ stock chain: decided only in load_middleware from settings
    # (never re-scanned on a request).
    _native_stock_chain = False
    _native_stock_specs = None
    # Per-path native classification frozen at load_middleware (debug/metrics).
    _middleware_native_at_load = None
    # path_info → (callback, args, kwargs, url_name, route) for lean C++ resolve.
    # Built once in load_middleware (empty / pure-stock / all-native hybrid).
    _exact_routes = None
    # True only when MIDDLEWARE is empty: C++ may skip the middleware chain.
    _lean_view_only = False
    # C++ owns WSGI outer loop (script prefix, request, start_response pack).
    # True for empty stack, pure stock chain, or all-native hybrid.
    _use_native_wsgi_outer = False

    # Middleware paths whose full request/response bodies can run in the pure
    # C++ stock chain (no Python process_request side effects required).
    _NATIVE_STOCK_CHAIN_TYPES = {
        "django.middleware.security.SecurityMiddleware": "security",
        "django.middleware.clickjacking.XFrameOptionsMiddleware": "xframe",
        "django.middleware.common.CommonMiddleware": "common",
    }

    def load_middleware(self, is_async=False):
        """
        Populate middleware lists from settings.MIDDLEWARE.

        Must be called after the environment is fixed (see __call__ in
        subclasses).

        Native-chain eligibility is resolved here (settings/handler init),
        not on the request path. The request loop only uses the frozen
        ``_middleware_chain`` / ``_native_stock_*`` state.
        """
        self._view_middleware = []
        self._template_response_middleware = []
        self._exception_middleware = []
        self._native_stock_chain = False
        self._native_stock_specs = None
        self._middleware_native_at_load = None
        self._exact_routes = None
        self._lean_view_only = False
        self._use_native_wsgi_outer = False

        get_response = self._get_response_async if is_async else self._get_response
        handler = convert_exception_to_response(get_response)
        handler_is_async = is_async
        from django import native as _native

        # --- Load-time native classification (settings.MIDDLEWARE once) -----
        # Every path is classified here. Request handling never re-inspects
        # the MIDDLEWARE list for stock-chain eligibility.
        stock_handler = self._install_native_stock_chain_if_eligible(
            get_response, is_async, _native
        )
        if stock_handler is not None:
            self._middleware_chain = stock_handler
            self._middleware_hooks_empty = True
            # Pure stock: C++ outer + native_stock_chain_call around lean view.
            self._lean_view_only = False
            self._use_native_wsgi_outer = bool(_native.AVAILABLE) and not is_async
            self._exact_routes = self._build_exact_route_table()
            self._pin_lean_urlconf()
            if self._use_native_wsgi_outer:
                from functools import partial

                self._stock_view = partial(
                    _native.wsgi_lean_get_response, self
                )
            return

        # Flattened hybrid: one plan for Security/Common/XFrame; Session/Auth/CSRF
        # stay thin Python. Avoids 6-deep MiddlewareMixin onion.
        hybrid_handler = self._install_hybrid_flattened_chain(
            get_response, is_async, _native
        )
        if hybrid_handler is not None:
            self._middleware_chain = hybrid_handler
            self._middleware_hooks_empty = not (
                self._view_middleware
                or self._template_response_middleware
                or self._exception_middleware
            )
            self._lean_view_only = False
            self._use_native_wsgi_outer = True
            self._exact_routes = self._build_exact_route_table()
            self._pin_lean_urlconf()
            return

        for middleware_path in reversed(settings.MIDDLEWARE):
            middleware = import_string(middleware_path)
            middleware_can_sync = getattr(middleware, "sync_capable", True)
            middleware_can_async = getattr(middleware, "async_capable", False)
            if _native.AVAILABLE:
                cap_ok = _native.middleware_capability_ok(
                    middleware_can_sync, middleware_can_async
                )
            else:
                cap_ok = middleware_can_sync or middleware_can_async
            if not cap_ok:
                raise RuntimeError(
                    "Middleware %s must have at least one of "
                    "sync_capable/async_capable set to True." % middleware_path
                )
            if not handler_is_async and middleware_can_sync:
                middleware_is_async = False
            else:
                middleware_is_async = middleware_can_async
            try:
                # Adapt handler, if needed.
                adapted_handler = self.adapt_method_mode(
                    middleware_is_async,
                    handler,
                    handler_is_async,
                    debug=settings.DEBUG,
                    name="middleware %s" % middleware_path,
                )
                mw_instance = middleware(adapted_handler)
            except MiddlewareNotUsed as exc:
                if settings.DEBUG:
                    if str(exc):
                        logger.debug("MiddlewareNotUsed(%r): %s", middleware_path, exc)
                    else:
                        logger.debug("MiddlewareNotUsed: %r", middleware_path)
                continue
            else:
                handler = adapted_handler

            if mw_instance is None:
                raise ImproperlyConfigured(
                    "Middleware factory %s returned None." % middleware_path
                )

            if hasattr(mw_instance, "process_view"):
                self._view_middleware.insert(
                    0,
                    self.adapt_method_mode(is_async, mw_instance.process_view),
                )
            if hasattr(mw_instance, "process_template_response"):
                self._template_response_middleware.append(
                    self.adapt_method_mode(
                        is_async, mw_instance.process_template_response
                    ),
                )
            if hasattr(mw_instance, "process_exception"):
                # The exception-handling stack is still always synchronous for
                # now, so adapt that way.
                self._exception_middleware.append(
                    self.adapt_method_mode(False, mw_instance.process_exception),
                )

            handler = convert_exception_to_response(mw_instance)
            handler_is_async = middleware_is_async

        # Adapt the top of the stack, if needed.
        handler = self.adapt_method_mode(is_async, handler, handler_is_async)
        # We only assign to this when initialization is complete as it is used
        # as a flag for initialization being complete.
        self._middleware_chain = handler
        self._middleware_hooks_empty = not (
            self._view_middleware
            or self._template_response_middleware
            or self._exception_middleware
        )

        paths = list(settings.MIDDLEWARE or ())
        truly_empty = not paths
        # All entries dual-path native (from classification built during install
        # attempt, or re-classify here for hybrid).
        if self._middleware_native_at_load is None:
            self._middleware_native_at_load = self._classify_middleware_at_load(
                paths, _native
            )
        all_native = bool(paths) and all(
            c.get("native") for c in self._middleware_native_at_load
        )

        # Empty stack: C++ may run view-only lean get_response.
        self._lean_view_only = truly_empty and not is_async
        # C++ WSGI outer loop for empty, pure-stock (handled above), or hybrid
        # where every middleware is stock dual-path native.
        self._use_native_wsgi_outer = bool(_native.AVAILABLE) and not is_async and (
            truly_empty or all_native or self._middleware_hooks_empty
        )

        # Exact routes + urlconf pin for empty, hooks-empty, and all-native hybrid.
        if self._use_native_wsgi_outer:
            self._exact_routes = self._build_exact_route_table()
            self._pin_lean_urlconf()

    def _pin_lean_urlconf(self):
        """
        Pin ROOT_URLCONF once at load so lean C++ get_response can skip
        per-request _ensure_root_urlconf (sync gunicorn workers are 1 thread).
        """
        try:
            set_urlconf(settings.ROOT_URLCONF)
            self._lean_urlconf_pinned = True
        except Exception:
            self._lean_urlconf_pinned = False

    def _build_exact_route_table(self):
        """
        Load-time map of path_info → view for converter-free path() routes.

        Includes one level of static ``include()`` prefixes (no converters on
        the include pattern). Used by lean C++ get_response to skip URLResolver
        on static endpoints (e.g. /plaintext, /api/health).
        """
        from django.urls import get_resolver
        from django.urls.resolvers import RoutePattern, URLPattern, URLResolver

        table = {}

        def add_pattern(prefix, pattern):
            if not isinstance(pattern, URLPattern):
                return
            route_pat = pattern.pattern
            if not isinstance(route_pat, RoutePattern):
                return
            if route_pat.converters:
                return
            route = str(route_pat)
            full = prefix + route
            path_info = full if full.startswith("/") else "/" + full
            table[path_info] = (
                pattern.callback,
                (),
                dict(pattern.default_args or {}),
                pattern.name,
                full.lstrip("/"),
            )

        try:
            resolver = get_resolver()
            for pattern in resolver.url_patterns:
                if isinstance(pattern, URLPattern):
                    add_pattern("", pattern)
                elif isinstance(pattern, URLResolver):
                    # One level of include: static RoutePattern prefix only.
                    inc = pattern.pattern
                    if not isinstance(inc, RoutePattern) or inc.converters:
                        continue
                    prefix = str(inc)
                    try:
                        for child in pattern.url_patterns:
                            add_pattern(prefix, child)
                    except Exception:
                        continue
        except Exception:
            return {}
        return table

    def _classify_middleware_at_load(self, paths, _native):
        """
        Classify each MIDDLEWARE dotted path once at load_middleware time.

        Returns a list of dicts:
          {"path": str, "native": bool, "chain_type": str|None}
        chain_type is set only for classes fully implementable by
        native_stock_chain_call (security|xframe|common).

        Never call this from the request path.
        """
        chain_types = self._NATIVE_STOCK_CHAIN_TYPES
        native_available = bool(getattr(_native, "AVAILABLE", False))
        out = []
        for path in paths:
            chain_type = chain_types.get(path)
            if native_available and chain_type is not None:
                is_native = True
            elif native_available and _native.is_native_stock_middleware_path(path):
                is_native = True
            else:
                # Fall back to class attribute for dual-path stock ports.
                try:
                    cls = import_string(path)
                    is_native = native_available and bool(
                        getattr(cls, "native_capable", False)
                    )
                except Exception:
                    is_native = False
            out.append(
                {
                    "path": path,
                    "native": is_native,
                    "chain_type": chain_type,
                }
            )
        return out

    def _build_native_stock_specs(self, classification):
        """
        Build frozen C++ chain specs from load-time classification + settings.

        Snapshots security/xframe settings now so the request path does not
        re-read settings for chain structure (config values are baked in).
        Returns None if the stack is not pure-C++-chain eligible.
        """
        if not classification:
            return None
        # Every link must be a pure-C++-chain type (not merely dual-path native).
        if any(c["chain_type"] is None for c in classification):
            return None
        # CommonMiddleware process_request (UA deny, APPEND_SLASH, PREPEND_WWW)
        # still needs Python / URL resolver — only pure-C++ when those are off.
        if any(c["chain_type"] == "common" for c in classification):
            if (
                settings.APPEND_SLASH
                or settings.PREPEND_WWW
                or settings.DISALLOWED_USER_AGENTS
            ):
                return None
        try:
            specs = []
            for c in classification:
                kind = c["chain_type"]
                if kind == "security":
                    specs.append(
                        {
                            "type": "security",
                            "redirect": bool(settings.SECURE_SSL_REDIRECT),
                            "redirect_host": settings.SECURE_SSL_HOST or "",
                            "exempt_patterns": list(
                                settings.SECURE_REDIRECT_EXEMPT or ()
                            ),
                            "sts_seconds": int(settings.SECURE_HSTS_SECONDS or 0),
                            "sts_include_subdomains": bool(
                                settings.SECURE_HSTS_INCLUDE_SUBDOMAINS
                            ),
                            "sts_preload": bool(settings.SECURE_HSTS_PRELOAD),
                            "content_type_nosniff": bool(
                                settings.SECURE_CONTENT_TYPE_NOSNIFF
                            ),
                            "referrer_policy": settings.SECURE_REFERRER_POLICY,
                            "cross_origin_opener_policy": (
                                settings.SECURE_CROSS_ORIGIN_OPENER_POLICY
                            ),
                        }
                    )
                elif kind == "xframe":
                    specs.append(
                        {
                            "type": "xframe",
                            "setting_value": getattr(
                                settings, "X_FRAME_OPTIONS", "DENY"
                            )
                            or "DENY",
                        }
                    )
                else:
                    specs.append({"type": "common"})
            return specs
        except Exception:
            return None

    def _install_native_stock_chain_if_eligible(
        self, get_response, is_async, _native
    ):
        """
        Load-time only: if every MIDDLEWARE entry is pure-C++-chainable,
        freeze specs on the handler and return a single chain callable.
        Otherwise freeze the classification for hybrid stacks and return None
        (Python walk + dual-path bodies).
        """
        paths = list(settings.MIDDLEWARE or ())
        classification = self._classify_middleware_at_load(paths, _native)
        self._middleware_native_at_load = classification

        if is_async or not getattr(_native, "AVAILABLE", False):
            self._native_stock_chain = False
            self._native_stock_specs = None
            return None

        specs = self._build_native_stock_specs(classification)
        if specs is None:
            self._native_stock_chain = False
            self._native_stock_specs = None
            return None

        # Freeze: request path uses only these attributes.
        self._native_stock_chain = True
        self._native_stock_specs = specs

        def stock_chain(request, *, _handler=self, _get_response=get_response, _n=_native):
            # No MIDDLEWARE / eligibility checks here — frozen at load.
            return _n.native_stock_chain_call(
                _handler._native_stock_specs, request, _get_response
            )

        return convert_exception_to_response(stock_chain)

    # Known hybrid middleware paths (process_request/response order in settings).
    _HYBRID_SESSION = "django.contrib.sessions.middleware.SessionMiddleware"
    _HYBRID_CSRF = "django.middleware.csrf.CsrfViewMiddleware"
    _HYBRID_AUTH = "django.contrib.auth.middleware.AuthenticationMiddleware"
    _HYBRID_SECURITY = "django.middleware.security.SecurityMiddleware"
    _HYBRID_COMMON = "django.middleware.common.CommonMiddleware"
    _HYBRID_XFRAME = "django.middleware.clickjacking.XFrameOptionsMiddleware"

    def _install_hybrid_flattened_chain(self, get_response, is_async, _native):
        """
        When every MIDDLEWARE entry is stock dual-path native and the stack
        includes Session/CSRF/Auth (cannot be pure C++ stock chain), install a
        single chain callable:

          hybrid_process_request (Security+Common)
          → Session/CSRF/Auth process_request (Python)
          → get_response (view middleware + view)
          → CSRF/Session process_response (Python)
          → hybrid_process_response (XFrame+Content-Length+Security)

        process_view hooks (CSRF) are registered on the handler as usual.
        """
        if is_async or not getattr(_native, "AVAILABLE", False):
            return None
        paths = list(settings.MIDDLEWARE or ())
        if not paths:
            return None
        classification = self._middleware_native_at_load
        if not classification or not all(c.get("native") for c in classification):
            return None
        path_set = set(paths)
        # Need at least one Python-only-body link; pure stock already handled.
        if not (
            self._HYBRID_SESSION in path_set
            or self._HYBRID_AUTH in path_set
            or self._HYBRID_CSRF in path_set
        ):
            return None
        # Only known stock paths (no custom middleware).
        known = {
            self._HYBRID_SECURITY,
            self._HYBRID_SESSION,
            self._HYBRID_COMMON,
            self._HYBRID_CSRF,
            self._HYBRID_AUTH,
            self._HYBRID_XFRAME,
            "django.middleware.gzip.GZipMiddleware",
            "django.middleware.http.ConditionalGetMiddleware",
            "django.contrib.auth.middleware.LoginRequiredMiddleware",
        }
        if any(p not in known for p in paths):
            return None
        # GZip still needs Python zlib mid-response — skip flatten if present.
        if "django.middleware.gzip.GZipMiddleware" in path_set:
            return None
        if "django.middleware.http.ConditionalGetMiddleware" in path_set:
            return None

        # Instantiate middleware for process_* only (get_response unused).
        def _noop(request):
            return None

        instances = {}
        for p in paths:
            cls = import_string(p)
            try:
                instances[p] = cls(_noop)
            except MiddlewareNotUsed:
                continue
            except Exception:
                return None

        session_mw = instances.get(self._HYBRID_SESSION)
        csrf_mw = instances.get(self._HYBRID_CSRF)
        auth_mw = instances.get(self._HYBRID_AUTH)

        # Register process_view / exception / template hooks.
        self._view_middleware = []
        self._template_response_middleware = []
        self._exception_middleware = []
        for p in paths:
            mw = instances.get(p)
            if mw is None:
                continue
            if hasattr(mw, "process_view"):
                self._view_middleware.append(
                    self.adapt_method_mode(is_async, mw.process_view)
                )
            if hasattr(mw, "process_template_response"):
                self._template_response_middleware.append(
                    self.adapt_method_mode(is_async, mw.process_template_response)
                )
            if hasattr(mw, "process_exception"):
                self._exception_middleware.append(
                    self.adapt_method_mode(False, mw.process_exception)
                )

        cfg = {
            "security": {
                "redirect": bool(settings.SECURE_SSL_REDIRECT),
                "redirect_host": settings.SECURE_SSL_HOST or "",
                "exempt_patterns": list(settings.SECURE_REDIRECT_EXEMPT or ()),
                "sts_seconds": int(settings.SECURE_HSTS_SECONDS or 0),
                "sts_include_subdomains": bool(
                    settings.SECURE_HSTS_INCLUDE_SUBDOMAINS
                ),
                "sts_preload": bool(settings.SECURE_HSTS_PRELOAD),
                "content_type_nosniff": bool(settings.SECURE_CONTENT_TYPE_NOSNIFF),
                "referrer_policy": settings.SECURE_REFERRER_POLICY,
                "cross_origin_opener_policy": (
                    settings.SECURE_CROSS_ORIGIN_OPENER_POLICY
                ),
            },
            "common": {
                "prepend_www": bool(settings.PREPEND_WWW),
                "content_length": True,
            },
            "xframe": {
                "setting_value": getattr(settings, "X_FRAME_OPTIONS", "DENY")
                or "DENY",
            },
        }
        # Drop sections for middleware not in the stack.
        if self._HYBRID_SECURITY not in path_set:
            del cfg["security"]
        if self._HYBRID_COMMON not in path_set:
            del cfg["common"]
        if self._HYBRID_XFRAME not in path_set:
            del cfg["xframe"]

        def hybrid_chain(
            request,
            *,
            _cfg=cfg,
            _session=session_mw,
            _csrf=csrf_mw,
            _auth=auth_mw,
            _get_response=get_response,
            _n=_native,
        ):
            early = _n.hybrid_process_request(_cfg, request)
            if early is not None:
                return early
            if _session is not None:
                _session.process_request(request)
            if _csrf is not None:
                _csrf.process_request(request)
            if _auth is not None:
                _auth.process_request(request)
            response = _get_response(request)
            if _csrf is not None:
                response = _csrf.process_response(request, response)
            if _session is not None:
                response = _session.process_response(request, response)
            return _n.hybrid_process_response(_cfg, request, response)

        self._hybrid_flattened = True
        return convert_exception_to_response(hybrid_chain)

    def _any_atomic_requests(self):
        """True if any DB alias has ATOMIC_REQUESTS (checked live; tests toggle it)."""
        try:
            return any(
                settings_dict.get("ATOMIC_REQUESTS")
                for settings_dict in connections.settings.values()
            )
        except Exception:
            return True

    def adapt_method_mode(
        self,
        is_async,
        method,
        method_is_async=None,
        debug=False,
        name=None,
    ):
        """
        Adapt a method to be in the correct "mode":
        - If is_async is False:
          - Synchronous methods are left alone
          - Asynchronous methods are wrapped with async_to_sync
        - If is_async is True:
          - Synchronous methods are wrapped with sync_to_async()
          - Asynchronous methods are left alone
        """
        if method_is_async is None:
            method_is_async = iscoroutinefunction(method)
        if debug and not name:
            name = name or "method %s()" % method.__qualname__
        if is_async:
            if not method_is_async:
                if debug:
                    logger.debug("Synchronous handler adapted for %s.", name)
                return sync_to_async(method, thread_sensitive=True)
        elif method_is_async:
            if debug:
                logger.debug("Asynchronous handler adapted for %s.", name)
            return async_to_sync(method)
        return method

    def _ensure_root_urlconf(self):
        """
        Pin ROOT_URLCONF on this thread if unset.

        Avoids set_urlconf thrash when the previous request left ROOT pinned
        (see reset_urlconf). reverse() and get_urlconf() still see a value.
        """
        from django.urls import get_urlconf

        if get_urlconf() is None:
            set_urlconf(settings.ROOT_URLCONF)

    def get_response(self, request):
        """Return an HttpResponse object for the given HttpRequest."""
        if not getattr(self, "_lean_urlconf_pinned", False):
            self._ensure_root_urlconf()
        response = self._middleware_chain(request)
        response._resource_closers.append(request.close)
        if response.status_code >= 400:
            log_response(
                "%s: %s",
                response.reason_phrase,
                request.path,
                response=response,
                request=request,
            )
        return response

    async def get_response_async(self, request):
        """
        Asynchronous version of get_response.

        Funneling everything, including WSGI, into a single async
        get_response() is too slow. Avoid the context switch by using
        a separate async response path.
        """
        self._ensure_root_urlconf()
        response = await self._middleware_chain(request)
        response._resource_closers.append(request.close)
        if response.status_code >= 400:
            await sync_to_async(log_response, thread_sensitive=False)(
                "%s: %s",
                response.reason_phrase,
                request.path,
                response=response,
                request=request,
            )
        return response

    def _get_response(self, request):
        """
        Resolve and call the view, then apply view, exception, and
        template_response middleware. This method is everything that happens
        inside the request/response middleware.
        """
        # Fast path: empty middleware hooks (TE-style MIDDLEWARE=()).
        if self._middleware_hooks_empty and not self._any_atomic_requests():
            callback, callback_args, callback_kwargs = self.resolve_request(request)
            if iscoroutinefunction(callback):
                callback = async_to_sync(callback)
            try:
                response = callback(request, *callback_args, **callback_kwargs)
            except Exception:
                raise
            self.check_response(response, callback)
            if hasattr(response, "render") and callable(response.render):
                response = response.render()
            return response

        response = None
        callback, callback_args, callback_kwargs = self.resolve_request(request)

        # Apply view middleware
        for middleware_method in self._view_middleware:
            response = middleware_method(
                request, callback, callback_args, callback_kwargs
            )
            if response:
                break

        if response is None:
            if self._any_atomic_requests():
                wrapped_callback = self.make_view_atomic(callback)
            else:
                wrapped_callback = callback
            # If it is an asynchronous view, run it in a subthread.
            if iscoroutinefunction(wrapped_callback):
                wrapped_callback = async_to_sync(wrapped_callback)
            try:
                response = wrapped_callback(request, *callback_args, **callback_kwargs)
            except Exception as e:
                response = self.process_exception_by_middleware(e, request)
                if response is None:
                    raise

        # Complain if the view returned None (a common error).
        self.check_response(response, callback)

        # If the response supports deferred rendering, apply template
        # response middleware and then render the response
        if hasattr(response, "render") and callable(response.render):
            for middleware_method in self._template_response_middleware:
                response = middleware_method(request, response)
                # Complain if the template response middleware returned None
                # (a common error).
                self.check_response(
                    response,
                    middleware_method,
                    name="%s.process_template_response"
                    % (middleware_method.__self__.__class__.__name__,),
                )
            try:
                response = response.render()
            except Exception as e:
                response = self.process_exception_by_middleware(e, request)
                if response is None:
                    raise

        return response

    async def _get_response_async(self, request):
        """
        Resolve and call the view, then apply view, exception, and
        template_response middleware. This method is everything that happens
        inside the request/response middleware.
        """
        response = None
        callback, callback_args, callback_kwargs = self.resolve_request(request)

        # Apply view middleware.
        for middleware_method in self._view_middleware:
            response = await middleware_method(
                request, callback, callback_args, callback_kwargs
            )
            if response:
                break

        if response is None:
            if self._any_atomic_requests():
                wrapped_callback = self.make_view_atomic(callback)
            else:
                wrapped_callback = callback
            # If it is a synchronous view, run it in a subthread
            if not iscoroutinefunction(wrapped_callback):
                wrapped_callback = sync_to_async(
                    wrapped_callback, thread_sensitive=True
                )
            try:
                response = await wrapped_callback(
                    request, *callback_args, **callback_kwargs
                )
            except Exception as e:
                response = await sync_to_async(
                    self.process_exception_by_middleware,
                    thread_sensitive=True,
                )(e, request)
                if response is None:
                    raise

        # Complain if the view returned None or an uncalled coroutine.
        self.check_response(response, callback)

        # If the response supports deferred rendering, apply template
        # response middleware and then render the response
        if hasattr(response, "render") and callable(response.render):
            for middleware_method in self._template_response_middleware:
                response = await middleware_method(request, response)
                # Complain if the template response middleware returned None or
                # an uncalled coroutine.
                self.check_response(
                    response,
                    middleware_method,
                    name="%s.process_template_response"
                    % (middleware_method.__self__.__class__.__name__,),
                )
            try:
                if iscoroutinefunction(response.render):
                    response = await response.render()
                else:
                    response = await sync_to_async(
                        response.render, thread_sensitive=True
                    )()
            except Exception as e:
                response = await sync_to_async(
                    self.process_exception_by_middleware,
                    thread_sensitive=True,
                )(e, request)
                if response is None:
                    raise

        # Make sure the response is not a coroutine
        if asyncio.iscoroutine(response):
            raise RuntimeError("Response is still a coroutine.")
        return response

    def resolve_request(self, request):
        """
        Retrieve/set the urlconf for the request. Return the view resolved,
        with its args and kwargs.
        """
        # Load-time exact-route table (empty / hybrid all-native / pure stock).
        # Skip full URLResolver for converter-free static paths.
        table = getattr(self, "_exact_routes", None)
        if table and not hasattr(request, "urlconf"):
            entry = table.get(request.path_info)
            if entry is not None:
                from django.urls.resolvers import ResolverMatch

                callback, args, kwargs, name, route = entry
                resolver_match = ResolverMatch(
                    callback, args, kwargs, name, route=route
                )
                request.resolver_match = resolver_match
                return resolver_match

        # Work out the resolver.
        if hasattr(request, "urlconf"):
            urlconf = request.urlconf
            set_urlconf(urlconf)
            resolver = get_resolver(urlconf)
        else:
            resolver = get_resolver()
        # Resolve the view, and assign the match object back to the request.
        resolver_match = resolver.resolve(request.path_info)
        request.resolver_match = resolver_match
        return resolver_match

    def check_response(self, response, callback, name=None):
        """
        Raise an error if the view returned None or an uncalled coroutine.
        """
        if not (response is None or asyncio.iscoroutine(response)):
            return
        if not name:
            if isinstance(callback, types.FunctionType):  # FBV
                name = "The view %s.%s" % (callback.__module__, callback.__name__)
            else:  # CBV
                name = "The view %s.%s.__call__" % (
                    callback.__module__,
                    callback.__class__.__name__,
                )
        if response is None:
            raise ValueError(
                "%s didn't return an HttpResponse object. It returned None "
                "instead." % name
            )
        elif asyncio.iscoroutine(response):
            raise ValueError(
                "%s didn't return an HttpResponse object. It returned an "
                "unawaited coroutine instead. You may need to add an 'await' "
                "into your view." % name
            )

    # Other utility methods.

    def make_view_atomic(self, view):
        if not self._any_atomic_requests():
            return view
        non_atomic_requests = getattr(view, "_non_atomic_requests", set())
        for alias, settings_dict in connections.settings.items():
            if settings_dict["ATOMIC_REQUESTS"] and alias not in non_atomic_requests:
                if iscoroutinefunction(view):
                    raise RuntimeError(
                        "You cannot use ATOMIC_REQUESTS with async views."
                    )
                view = transaction.atomic(using=alias)(view)
        return view

    def process_exception_by_middleware(self, exception, request):
        """
        Pass the exception to the exception middleware. If no middleware
        return a response for this exception, return None.
        """
        for middleware_method in self._exception_middleware:
            response = middleware_method(request, exception)
            if response:
                return response
        return None


def reset_urlconf(sender, **kwargs):
    """
    Reset the URLconf after each request is finished.

    Custom per-request urlconfs (request.urlconf) are cleared so they do not
    leak to the next request. ROOT_URLCONF left pinned on the thread is kept to
    avoid set/clear thrash on the framework-floor path.
    """
    from django.urls import get_urlconf

    urlconf = get_urlconf()
    if urlconf is None:
        return
    if urlconf != settings.ROOT_URLCONF:
        set_urlconf(None)
    # else: leave ROOT_URLCONF pinned for the next request


request_finished.connect(reset_urlconf)
