import os
import sys
import importlib
from unittest import mock

from django.test import SimpleTestCase
from django.utils.safestring import SafeString


def _reload_native_stack():
    for name in (
        "django.native",
        "django.native._loader",
        "django.native.fallbacks",
    ):
        sys.modules.pop(name, None)
    import django.native.fallbacks  # noqa: F401
    import django.native._loader  # noqa: F401

    return importlib.import_module("django.native")


class NativeHtmlEscapeTests(SimpleTestCase):
    def test_html_escape_basic(self):
        from django import native

        self.assertTrue(native.AVAILABLE)
        cases = (
            ("&", "&amp;"),
            ("<", "&lt;"),
            (">", "&gt;"),
            ('"', "&quot;"),
            ("'", "&#x27;"),
            ("a&b", "a&amp;b"),
            ("<&", "&lt;&amp;"),
            ("plain", "plain"),
            ("", ""),
            ("café <bar>", "café &lt;bar&gt;"),
        )
        for value, expected in cases:
            with self.subTest(value=value):
                self.assertEqual(native.html_escape(value), expected)
                self.assertEqual(native.fallbacks.html_escape(value), expected)

    def test_utils_escape_uses_native_and_is_safe(self):
        from django.utils.html import escape

        result = escape("<script>alert('x')</script>")
        self.assertEqual(
            result,
            "&lt;script&gt;alert(&#x27;x&#x27;)&lt;/script&gt;",
        )
        self.assertIsInstance(result, SafeString)

    def test_escapejs_basic(self):
        from django import native

        value = "\"quotes\" and 'apostrophes' and <tag> &\n\u2028"
        expected = (
            "\\u0022quotes\\u0022 and \\u0027apostrophes\\u0027 and "
            "\\u003Ctag\\u003E \\u0026\\u000A\\u2028"
        )
        self.assertEqual(native.escapejs(value), expected)
        self.assertEqual(native.fallbacks.escapejs(value), expected)

    def test_html_escape_fallback_when_disabled(self):
        with mock.patch.dict(os.environ, {"DJANGO_NATIVE": "0"}, clear=False):
            native = _reload_native_stack()
            self.assertFalse(native.AVAILABLE)
            self.assertEqual(native.html_escape("a<b>"), "a&lt;b&gt;")
            self.assertEqual(native.escapejs("<"), "\\u003C")

        with mock.patch.dict(os.environ, {"DJANGO_NATIVE": "1"}, clear=False):
            restored = _reload_native_stack()
            self.assertTrue(restored.AVAILABLE)
