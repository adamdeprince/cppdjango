import re

from django.conf import settings
from django.http import HttpResponsePermanentRedirect
from django.utils.deprecation import MiddlewareMixin


class SecurityMiddleware(MiddlewareMixin):
    def __init__(self, get_response):
        super().__init__(get_response)
        self.sts_seconds = settings.SECURE_HSTS_SECONDS
        self.sts_include_subdomains = settings.SECURE_HSTS_INCLUDE_SUBDOMAINS
        self.sts_preload = settings.SECURE_HSTS_PRELOAD
        self.content_type_nosniff = settings.SECURE_CONTENT_TYPE_NOSNIFF
        self.redirect = settings.SECURE_SSL_REDIRECT
        self.redirect_host = settings.SECURE_SSL_HOST
        self.redirect_exempt = [re.compile(r) for r in settings.SECURE_REDIRECT_EXEMPT]
        self.referrer_policy = settings.SECURE_REFERRER_POLICY
        self.cross_origin_opener_policy = settings.SECURE_CROSS_ORIGIN_OPENER_POLICY

    def process_request(self, request):
        from django import native as _native

        path = request.path.lstrip("/")
        if (
            self.redirect
            and not request.is_secure()
            and not any(pattern.search(path) for pattern in self.redirect_exempt)
        ):
            host = self.redirect_host or request.get_host()
            if _native.AVAILABLE:
                url = _native.https_redirect_url(host, request.get_full_path())
            else:
                url = "https://%s%s" % (host, request.get_full_path())
            return HttpResponsePermanentRedirect(url)

    def process_response(self, request, response):
        from django import native as _native

        if (
            self.sts_seconds
            and request.is_secure()
            and "Strict-Transport-Security" not in response
        ):
            if _native.AVAILABLE:
                sts_header = _native.hsts_header_value(
                    self.sts_seconds,
                    self.sts_include_subdomains,
                    self.sts_preload,
                )
            else:
                sts_header = "max-age=%s" % self.sts_seconds
                if self.sts_include_subdomains:
                    sts_header += "; includeSubDomains"
                if self.sts_preload:
                    sts_header += "; preload"
            response.headers["Strict-Transport-Security"] = sts_header

        if self.content_type_nosniff:
            response.headers.setdefault("X-Content-Type-Options", "nosniff")

        if self.referrer_policy:
            # Support a comma-separated string or iterable of values to allow
            # fallback.
            if isinstance(self.referrer_policy, str):
                parts = [v.strip() for v in self.referrer_policy.split(",")]
            else:
                parts = list(self.referrer_policy)
            if _native.AVAILABLE:
                policy = _native.referrer_policy_header(parts)
            else:
                policy = ",".join(parts)
            response.headers.setdefault("Referrer-Policy", policy)

        if self.cross_origin_opener_policy:
            response.setdefault(
                "Cross-Origin-Opener-Policy",
                self.cross_origin_opener_policy,
            )
        return response
