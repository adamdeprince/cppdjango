import uuid

from django.test import SimpleTestCase, override_settings
from django.urls import path, resolve, reverse
from django.urls.resolvers import RoutePattern


def dummy_view(request, **kwargs):
    return None


class NativeRouteMatchTests(SimpleTestCase):
    def test_compile_and_match_int_str_slug_uuid(self):
        from django import native

        self.assertTrue(native.AVAILABLE)
        compiled = native.compile_route(
            "articles/<int:pk>/slug/<slug:slug>/user/<uuid:uid>/",
            is_endpoint=True,
        )
        self.assertIsNotNone(compiled)
        uid = "39da9369-838e-4750-91a5-f7805cd82839"
        matched = native.match_route(
            compiled,
            f"articles/42/slug/hello-world/user/{uid}/",
        )
        self.assertIsNotNone(matched)
        remaining, kwargs = matched
        self.assertEqual(remaining, "")
        self.assertEqual(kwargs["pk"], 42)
        self.assertEqual(kwargs["slug"], "hello-world")
        self.assertEqual(kwargs["uid"], uuid.UUID(uid))

    def test_path_converter_backtracking(self):
        from django import native

        compiled = native.compile_route("files/<path:p>/meta/", is_endpoint=True)
        matched = native.match_route(compiled, "files/a/b/c/meta/")
        self.assertIsNotNone(matched)
        remaining, kwargs = matched
        self.assertEqual(remaining, "")
        self.assertEqual(kwargs, {"p": "a/b/c"})

    def test_custom_converter_returns_none(self):
        from django import native

        self.assertIsNone(native.compile_route("x/<base64:data>/", is_endpoint=True))

    def test_route_pattern_uses_native(self):
        pattern = RoutePattern("items/<int:pk>/", name="item", is_endpoint=True)
        self.assertIsNotNone(pattern._native_route)
        self.assertEqual(
            pattern.match("items/7/"),
            ("", (), {"pk": 7}),
        )
        self.assertIsNone(pattern.match("items/nope/"))

    def test_converters_int_uuid(self):
        from django import native

        self.assertEqual(native.converter_int_to_python("01"), 1)
        self.assertEqual(native.converter_int_to_url(99), "99")
        u = native.converter_uuid_to_python("39da9369-838e-4750-91a5-f7805cd82839")
        self.assertIsInstance(u, uuid.UUID)
        self.assertEqual(str(u), "39da9369-838e-4750-91a5-f7805cd82839")


# Minimal urlpatterns for resolve checks (ROOT_URLCONF points here).
urlpatterns = [
    path("int/<int:int>/", dummy_view, name="int"),
    path("str/<str:str>/", dummy_view, name="str"),
    path("slug/<slug:slug>/", dummy_view, name="slug"),
    path("path/<path:path>/", dummy_view, name="path"),
    path("uuid/<uuid:uuid>/", dummy_view, name="uuid"),
]


@override_settings(ROOT_URLCONF="native_scaffold.test_urls")
class NativeResolveTests(SimpleTestCase):
    def test_resolve_converters(self):
        match = resolve("/int/123/")
        self.assertEqual(match.kwargs, {"int": 123})
        match = resolve("/slug/abc-XYZ_1/")
        self.assertEqual(match.kwargs, {"slug": "abc-XYZ_1"})
        uid = "39da9369-838e-4750-91a5-f7805cd82839"
        match = resolve(f"/uuid/{uid}/")
        self.assertEqual(match.kwargs, {"uuid": uuid.UUID(uid)})
        self.assertEqual(reverse("int", kwargs={"int": 5}), "/int/5/")
