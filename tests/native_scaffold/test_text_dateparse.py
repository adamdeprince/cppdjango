from datetime import date, datetime, time, timedelta

from django.test import SimpleTestCase
from django.utils.dateparse import (
    parse_date,
    parse_datetime,
    parse_duration,
    parse_time,
)
from django.utils.text import get_valid_filename, slugify


class NativeTextTests(SimpleTestCase):
    def test_slugify_ascii_and_unicode(self):
        from django import native

        self.assertTrue(native.AVAILABLE)
        self.assertEqual(slugify("Hello, World!"), "hello-world")
        self.assertEqual(slugify("spam & ıçüş", allow_unicode=True), "spam-ıçüş")
        self.assertEqual(native.slugify("multiple---dash"), "multiple-dash")
        self.assertEqual(native.fallbacks.slugify("Hello, World!"), "hello-world")

    def test_get_valid_filename(self):
        from django import native

        self.assertEqual(get_valid_filename("a b.txt"), "a_b.txt")
        self.assertEqual(
            native.get_valid_filename("^&'@{}[],$=!-#()%+~_123.txt"),
            "-_123.txt",
        )
        self.assertIsNone(native.get_valid_filename("???"))


class NativeDateParseTests(SimpleTestCase):
    def test_parse_date_time_datetime(self):
        self.assertEqual(parse_date("2012-4-9"), date(2012, 4, 9))
        self.assertEqual(parse_time("10:20:30.400"), time(10, 20, 30, 400000))
        self.assertEqual(
            parse_datetime("2012-4-9 4:8:16"),
            datetime(2012, 4, 9, 4, 8, 16),
        )

    def test_parse_duration_formats(self):
        from django import native

        self.assertEqual(parse_duration("P4D"), timedelta(days=4))
        self.assertEqual(
            parse_duration("4 10:15:30"),
            timedelta(days=4, hours=10, minutes=15, seconds=30),
        )
        self.assertEqual(native.parse_duration("PT5S"), timedelta(seconds=5))
        self.assertEqual(
            native.fallbacks.parse_duration("15:30"),
            timedelta(minutes=15, seconds=30),
        )
