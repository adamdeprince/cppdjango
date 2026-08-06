"""
Clickjacking Protection Middleware.

This module provides a middleware that implements protection against a
malicious site loading resources from your site in a hidden frame.
"""

from django import native as _native
from django.conf import settings
from django.utils.deprecation import MiddlewareMixin


class XFrameOptionsMiddleware(MiddlewareMixin):
    """
    Set the X-Frame-Options HTTP header in HTTP responses.

    Dual-path: skip decision can use native; value via get_xframe_options_value
    (subclass-friendly). Header uppercasing uses native when available.
    """

    native_capable = True

    def __init__(self, get_response):
        super().__init__(get_response)
        self._use_native = _native.AVAILABLE

    def process_response(self, request, response):
        # Don't set it if it's already in the response
        if response.get("X-Frame-Options") is not None:
            return response

        # Don't set it if they used @xframe_options_exempt
        if getattr(response, "xframe_options_exempt", False):
            return response

        response.headers["X-Frame-Options"] = self.get_xframe_options_value(
            request,
            response,
        )
        return response

    def get_xframe_options_value(self, request, response):
        """
        Get the value to set for the X_FRAME_OPTIONS header. Use the value from
        the X_FRAME_OPTIONS setting, or 'DENY' if not set.

        This method can be overridden if needed, allowing it to vary based on
        the request or response.
        """
        raw = getattr(settings, "X_FRAME_OPTIONS", "DENY")
        if getattr(self, "_use_native", _native.AVAILABLE):
            return _native.xframe_options_value(raw or "")
        return (raw or "DENY").upper()
