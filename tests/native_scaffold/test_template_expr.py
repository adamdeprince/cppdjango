from django.template.base import FilterExpression, Parser, Variable
from django.template.defaultfilters import register as filter_library
from django.test import SimpleTestCase
from django.utils.text import smart_split, unescape_string_literal


class NativeTemplateExprTests(SimpleTestCase):
    def test_smart_split(self):
        self.assertEqual(
            list(smart_split(r'This is "a person\'s" test.')),
            ["This", "is", '"a person\\\'s"', "test."],
        )

    def test_unescape_string_literal(self):
        self.assertEqual(unescape_string_literal(r'"Some \"Good\" News"'), 'Some "Good" News')

    def test_variable_and_filter_expression(self):
        from django import native

        self.assertTrue(native.AVAILABLE)
        c = {"article": {"section": "News"}}
        self.assertEqual(Variable("article.section").resolve(c), "News")
        p = Parser("", builtins=[filter_library])
        self.assertEqual(FilterExpression("article.section|upper", p).resolve(c), "NEWS")
        ok, val = native.resolve_dict_lookups(c, ["article", "section"])
        self.assertTrue(ok)
        self.assertEqual(val, "News")
