from django.http import QueryDict
from django.test import SimpleTestCase, override_settings


class NativeParseQslTests(SimpleTestCase):
    def test_parse_qsl_basic(self):
        from django import native

        self.assertTrue(native.AVAILABLE)
        self.assertEqual(
            native.parse_qsl("foo=bar&vote=yes&vote=no"),
            [("foo", "bar"), ("vote", "yes"), ("vote", "no")],
        )
        self.assertEqual(
            native.parse_qsl("a=b+c&d=e%20f"),
            [("a", "b c"), ("d", "e f")],
        )
        self.assertEqual(native.parse_qsl("key1&key2="), [("key1", ""), ("key2", "")])
        self.assertEqual(native.parse_qsl("&&a=1&"), [("a", "1")])
        self.assertEqual(native.parse_qsl(""), [])
        self.assertEqual(native.parse_qsl("a=b=c"), [("a", "b=c")])
        self.assertEqual(native.parse_qsl("=b"), [("", "b")])

    def test_parse_qsl_utf8_and_fallback_match(self):
        from django import native

        samples = [
            "name=John%20Doe&age=30",
            "a=%E2%9C%93",
            "x=1&x=2&x=",
            "foo%00bar=1",
            "a=b&c=d&a=1",
        ]
        for qs in samples:
            with self.subTest(qs=qs):
                self.assertEqual(
                    native.parse_qsl(qs),
                    native.fallbacks.parse_qsl(qs),
                )

    def test_max_num_fields(self):
        from django import native

        with self.assertRaises(ValueError):
            native.parse_qsl("a=1&b=2&c=3", max_num_fields=2)
        # CPython counts 1 + number of separators, including empties.
        with self.assertRaises(ValueError):
            native.parse_qsl("&&a=1&", max_num_fields=3)
        self.assertEqual(native.parse_qsl("a=1&b=2", max_num_fields=2), [("a", "1"), ("b", "2")])

    def test_querydict_uses_native(self):
        q = QueryDict("vote=yes&vote=no&name=Ada+Lovelace")
        self.assertEqual(q.getlist("vote"), ["yes", "no"])
        self.assertEqual(q["name"], "Ada Lovelace")

    def test_querydict_non_utf8_still_works(self):
        q = QueryDict("cur=%A4", encoding="iso-8859-15")
        self.assertEqual(q["cur"], "€")

    @override_settings(DATA_UPLOAD_MAX_NUMBER_FIELDS=2)
    def test_querydict_too_many_fields(self):
        from django.core.exceptions import TooManyFieldsSent

        with self.assertRaises(TooManyFieldsSent):
            QueryDict("a=1&b=2&c=3")
