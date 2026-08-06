import time
from importlib import import_module

from django import native as _native
from django.conf import settings
from django.contrib.sessions.backends.base import UpdateError
from django.contrib.sessions.exceptions import SessionInterrupted
from django.utils.cache import patch_vary_headers
from django.utils.deprecation import MiddlewareMixin
from django.utils.http import http_date


class SessionMiddleware(MiddlewareMixin):
    """
    Dual-path: session key validation and process_response plan in C++.
    SessionStore construction/save remains Python (engine-specific).
    """

    native_capable = True

    def __init__(self, get_response):
        super().__init__(get_response)
        engine = import_module(settings.SESSION_ENGINE)
        self.SessionStore = engine.SessionStore
        self._use_native = _native.AVAILABLE

    def process_request(self, request):
        # Avoid full COOKIES parse when the header is absent (hybrid TE path).
        if not request.META.get("HTTP_COOKIE"):
            request.session = self.SessionStore(None)
            return
        session_key = request.COOKIES.get(settings.SESSION_COOKIE_NAME)
        if session_key is None:
            request.session = self.SessionStore(None)
            return
        if self._use_native:
            session_key = _native.session_load_key(session_key)
        request.session = self.SessionStore(session_key)

    def process_response(self, request, response):
        """
        If request.session was modified, or if the configuration is to save the
        session every time, save the changes and set a session cookie or delete
        the session cookie if the session has been emptied.
        """
        try:
            accessed = request.session.accessed
            modified = request.session.modified
        except AttributeError:
            return response

        # Fast no-op: session never touched and we do not save every request.
        # Skips is_empty() / native plan / Vary work (hybrid TE path).
        save_every = bool(settings.SESSION_SAVE_EVERY_REQUEST)
        if self._use_native:
            if not _native.session_response_needs_work(
                bool(accessed), bool(modified), save_every
            ):
                return response
        elif not accessed and not modified and not save_every:
            return response

        try:
            empty = request.session.is_empty()
        except AttributeError:
            return response

        cookie_name = settings.SESSION_COOKIE_NAME
        if self._use_native:
            # Avoid get_expiry_* until we know we may save — those touch the
            # session dict and would load from the DB on every response.
            may_save = (modified or save_every) and not empty
            if may_save:
                expire_browser = bool(request.session.get_expire_at_browser_close())
                expiry_age = int(request.session.get_expiry_age() or 0)
            else:
                expire_browser = True
                expiry_age = 0
            plan = _native.session_process_response_plan(
                bool(accessed),
                bool(modified),
                bool(empty),
                cookie_name in request.COOKIES,
                save_every,
                int(response.status_code),
                expire_browser,
                expiry_age,
                time.time(),
            )
            action = plan.get("action")
            need_vary_cookie = bool(plan.get("need_vary"))
            if action == "delete":
                response.delete_cookie(
                    cookie_name,
                    path=settings.SESSION_COOKIE_PATH,
                    domain=settings.SESSION_COOKIE_DOMAIN,
                    samesite=settings.SESSION_COOKIE_SAMESITE,
                )
            elif action == "save" and plan.get("saveable"):
                try:
                    request.session.save()
                except UpdateError:
                    raise SessionInterrupted(
                        "The request's session was deleted before the "
                        "request completed. The user may have logged "
                        "out in a concurrent request, for example."
                    )
                response.set_cookie(
                    cookie_name,
                    request.session.session_key,
                    max_age=plan.get("max_age"),
                    expires=plan.get("expires"),
                    domain=settings.SESSION_COOKIE_DOMAIN,
                    path=settings.SESSION_COOKIE_PATH,
                    secure=settings.SESSION_COOKIE_SECURE or None,
                    httponly=settings.SESSION_COOKIE_HTTPONLY or None,
                    samesite=settings.SESSION_COOKIE_SAMESITE,
                )
                need_vary_cookie = True
            if need_vary_cookie:
                patch_vary_headers(response, ("Cookie",))
            return response

        # Pure-Python path
        if cookie_name in request.COOKIES and empty:
            response.delete_cookie(
                cookie_name,
                path=settings.SESSION_COOKIE_PATH,
                domain=settings.SESSION_COOKIE_DOMAIN,
                samesite=settings.SESSION_COOKIE_SAMESITE,
            )
            need_vary_cookie = True
        else:
            need_vary_cookie = accessed
            if (modified or save_every) and not empty:
                if request.session.get_expire_at_browser_close():
                    max_age = None
                    expires = None
                else:
                    max_age = request.session.get_expiry_age()
                    expires_time = time.time() + max_age
                    expires = http_date(expires_time)
                saveable = response.status_code < 500
                if saveable:
                    try:
                        request.session.save()
                    except UpdateError:
                        raise SessionInterrupted(
                            "The request's session was deleted before the "
                            "request completed. The user may have logged "
                            "out in a concurrent request, for example."
                        )
                    response.set_cookie(
                        cookie_name,
                        request.session.session_key,
                        max_age=max_age,
                        expires=expires,
                        domain=settings.SESSION_COOKIE_DOMAIN,
                        path=settings.SESSION_COOKIE_PATH,
                        secure=settings.SESSION_COOKIE_SECURE or None,
                        httponly=settings.SESSION_COOKIE_HTTPONLY or None,
                        samesite=settings.SESSION_COOKIE_SAMESITE,
                    )
                    need_vary_cookie = True
        if need_vary_cookie:
            patch_vary_headers(response, ("Cookie",))
        return response
