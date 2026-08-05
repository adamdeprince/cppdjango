import importlib
import os
import sys
from unittest import mock

from django.test import SimpleTestCase


def _reload_native_stack():
    """Reload loader + facade so env / import changes take effect."""
    for name in (
        "django.native",
        "django.native._loader",
        "django.native.fallbacks",
    ):
        sys.modules.pop(name, None)
    import django.native.fallbacks  # noqa: F401
    import django.native._loader  # noqa: F401

    return importlib.import_module("django.native")


class NativeScaffoldTests(SimpleTestCase):
    def test_native_module_available(self):
        from django import native

        self.assertTrue(
            native.AVAILABLE,
            "django._native extension failed to import; build the native layer",
        )
        self.assertFalse(native.DISABLED_BY_ENV)
        self.assertTrue(native.is_native_enabled())
        self.assertEqual(native.add(2, 40), 42)
        self.assertEqual(native.cxx_standard(), "c++26")
        self.assertTrue(native.compiler())
        self.assertNotEqual(native.compiler(), "none")
        self.assertEqual(native.version(), "6.0.7")
        self.assertIsNotNone(native.get_native_module())

    def test_direct_extension_import(self):
        from django import _native

        self.assertTrue(_native.AVAILABLE)
        self.assertEqual(_native.add(1, 2), 3)
        self.assertEqual(_native.cxx_standard(), "c++26")
        self.assertIn("g++", _native.compiler())
        self.assertEqual(_native.version(), "6.0.7")

    def test_facade_matches_extension(self):
        from django import _native, native

        self.assertEqual(native.add(7, 9), _native.add(7, 9))
        self.assertEqual(native.version(), _native.version())
        self.assertEqual(native.cxx_standard(), _native.cxx_standard())
        self.assertEqual(native.compiler(), _native.compiler())


class NativeFallbackTests(SimpleTestCase):
    def test_pure_python_fallbacks_api(self):
        from django.native import fallbacks

        self.assertEqual(fallbacks.add(2, 40), 42)
        self.assertEqual(fallbacks.cxx_standard(), "python")
        self.assertEqual(fallbacks.compiler(), "none")
        self.assertEqual(fallbacks.version(), "6.0.7")

    def test_env_disables_native(self):
        with mock.patch.dict(os.environ, {"DJANGO_NATIVE": "0"}, clear=False):
            native = _reload_native_stack()
            self.assertFalse(native.AVAILABLE)
            self.assertTrue(native.DISABLED_BY_ENV)
            self.assertFalse(native.is_native_enabled())
            self.assertIsNone(native.get_native_module())
            self.assertEqual(native.add(2, 40), 42)
            self.assertEqual(native.cxx_standard(), "python")
            self.assertEqual(native.compiler(), "none")
            self.assertEqual(native.version(), "6.0.7")

        with mock.patch.dict(os.environ, {"DJANGO_NATIVE": "1"}, clear=False):
            restored = _reload_native_stack()
            self.assertTrue(restored.AVAILABLE)

    def test_missing_extension_uses_fallbacks(self):
        real_import_module = importlib.import_module

        def fake_import_module(name, package=None):
            if name == "django._native":
                raise ImportError("simulated missing extension")
            return real_import_module(name, package)

        with mock.patch.dict(os.environ, {"DJANGO_NATIVE": "1"}, clear=False):
            with mock.patch("importlib.import_module", side_effect=fake_import_module):
                native = _reload_native_stack()
                self.assertFalse(native.AVAILABLE)
                self.assertEqual(native.add(3, 4), 7)
                self.assertEqual(native.cxx_standard(), "python")
                self.assertEqual(native.compiler(), "none")

        with mock.patch.dict(os.environ, {"DJANGO_NATIVE": "1"}, clear=False):
            restored = _reload_native_stack()
            self.assertTrue(
                restored.AVAILABLE,
                "native extension should load again after import patch ends",
            )
