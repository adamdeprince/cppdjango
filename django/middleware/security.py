import re

from django import native as _native
from django.conf import settings
from django.http import HttpResponsePermanentRedirect
from django.utils.deprecation import MiddlewareMixin


class SecurityMiddleware(MiddlewareMixin):
    """
    Security headers and optional HTTPS redirect.

    Dual-path: process_request / process_response bodies run in C++ when
    native is available (one crossing per method). Chain iteration stays
    in Python (unless the whole stack is pure stock and uses the C++ chain).
    """

    native_capable = True

    def __init__(self, get_response):
        super().__init__(get_response)
        self.sts_seconds = settings.SECURE_HSTS_SECONDS
        self.sts_include_subdomains = settings.SECURE_HSTS_INCLUDE_SUBDOMAINS
        self.sts_preload = settings.SECURE_HSTS_PRELOAD
        self.content_type_nosniff = settings.SECURE_CONTENT_TYPE_NOSNIFF
        self.redirect = settings.SECURE_SSL_REDIRECT
        self.redirect_host = settings.SECURE_SSL_HOST
        self.redirect_exempt = [re.compile(r) for r in settings.SECURE_REDIRECT_EXEMPT]
        self._redirect_exempt_patterns = list(settings.SECURE_REDIRECT_EXEMPT)
        self.referrer_policy = settings.SECURE_REFERRER_POLICY
        self.cross_origin_opener_policy = settings.SECURE_CROSS_ORIGIN_OPENER_POLICY
        # Freeze dual-path choice at load (not per request).
        self._use_native = _native.AVAILABLE

    def process_request(self, request):
        if self._use_native:
            url = _native.security_process_request(
                self.redirect,
                request.is_secure(),
                request.path.lstrip("/"),
                request.get_full_path(),
                self.redirect_host or "",
                request.get_host(),
                self._redirect_exempt_patterns,
            )
            if url is not None:
                return HttpResponsePermanentRedirect(url)
            return None

        path = request.path.lstrip("/")
        if (
            self.redirect
            and not request.is_secure()
            and not any(pattern.search(path) for pattern in self.redirect_exempt)
        ):
            host = self.redirect_host or request.get_host()
            url = "https://%s%s" % (host, request.get_full_path())
            return HttpResponsePermanentRedirect(url)

    def process_response(self, request, response):
        if self._use_native:
            actions = _native.security_process_response(
                request.is_secure(),
                "Strict-Transport-Security" in response,
                int(self.sts_seconds or 0),
                bool(self.sts_include_subdomains),
                bool(self.sts_preload),
                bool(self.content_type_nosniff),
                "X-Content-Type-Options" in response,
                self.referrer_policy if self.referrer_policy else None,
                "Referrer-Policy" in response,
                self.cross_origin_opener_policy
                if self.cross_origin_opener_policy
                else None,
                "Cross-Origin-Opener-Policy" in response,
            )
            for name, value in (actions.get("set") or {}).items():
                response.headers[name] = value
            for name, value in (actions.get("setdefault") or {}).items():
                response.headers.setdefault(name, value)
            return response

        if (
            self.sts_seconds
            and request.is_secure()
            and "Strict-Transport-Security" not in response
        ):
            sts_header = "max-age=%s" % self.sts_seconds
            if self.sts_include_subdomains:
                sts_header += "; includeSubDomains"
            if self.sts_preload:
                sts_header += "; preload"
            response.headers["Strict-Transport-Security"] = sts_header

        if self.content_type_nosniff:
            response.headers.setdefault("X-Content-Type-Options", "nosniff")

        if self.referrer_policy:
            if isinstance(self.referrer_policy, str):
                parts = [v.strip() for v in self.referrer_policy.split(",")]
            else:
                parts = list(self.referrer_policy)
            response.headers.setdefault("Referrer-Policy", ",".join(parts))

        if self.cross_origin_opener_policy:
            response.setdefault(
                "Cross-Origin-Opener-Policy",
                self.cross_origin_opener_policy,
            )
        return response
