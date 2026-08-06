"""
Correctness harness for Tier-1+ ORM fast paths (native on and DJANGO_NATIVE=0).

Covers TE-shaped patterns: values_list.get, model get, values fetch, in_bulk,
filter().update, and fortune HTML rendering.
"""

import os
from unittest import mock

from django.test import SimpleTestCase, TestCase, modify_settings
from django.test.utils import isolate_apps

from .models import FastFortune, FastWorld


@isolate_apps("native_orm_fastpath")
class SimpleSQLHelpersTests(SimpleTestCase):
    def test_simple_update_eq_sql_shape(self):
        from django import native

        sql = native.simple_update_eq_sql('"world"', ['"randomnumber"'], '"id"')
        self.assertEqual(
            sql, 'UPDATE "world" SET "randomnumber" = %s WHERE "id" = %s'
        )

    def test_render_fortune_page_escapes_and_structure(self):
        from django import native

        html = native.render_fortune_page(
            [(1, "alpha"), (0, "Additional <b>x</b> & y"), (2, "zzz")]
        )
        self.assertIn("<title>Fortunes</title>", html)
        self.assertIn("<th>id</th>", html)
        self.assertIn("<th>message</th>", html)
        self.assertIn("<td>0</td>", html)
        self.assertIn("Additional &lt;b&gt;x&lt;/b&gt; &amp; y", html)
        self.assertNotIn("<b>x</b>", html)
        # Fallback and native must agree.
        from django.native import fallbacks

        fb = fallbacks.render_fortune_page(
            [(1, "alpha"), (0, "Additional <b>x</b> & y"), (2, "zzz")]
        )
        if native.AVAILABLE:
            self.assertEqual(html, fb)

    def test_fallback_update_sql(self):
        from django.native import fallbacks

        sql = fallbacks.simple_update_eq_sql('"t"', ['"a"', '"b"'], '"id"')
        self.assertEqual(sql, 'UPDATE "t" SET "a" = %s, "b" = %s WHERE "id" = %s')


@isolate_apps("native_orm_fastpath")
@modify_settings(INSTALLED_APPS={"append": "native_orm_fastpath"})
class OrmFastPathDBTests(TestCase):
    @classmethod
    def setUpTestData(cls):
        FastWorld.objects.bulk_create(
            [FastWorld(id=i, randomnumber=i * 10) for i in range(1, 21)]
        )
        FastFortune.objects.bulk_create(
            [
                FastFortune(id=1, message="gamma"),
                FastFortune(id=2, message="alpha"),
                FastFortune(id=3, message="beta <x>"),
            ]
        )

    def test_values_list_get(self):
        row = FastWorld.objects.values_list("id", "randomnumber").get(id=7)
        self.assertEqual(row, (7, 70))

    def test_values_list_get_missing(self):
        with self.assertRaises(FastWorld.DoesNotExist):
            FastWorld.objects.values_list("id", "randomnumber").get(id=9999)

    def test_model_get_pk(self):
        obj = FastWorld.objects.get(pk=3)
        self.assertEqual(obj.id, 3)
        self.assertEqual(obj.randomnumber, 30)

    def test_model_get_field(self):
        obj = FastWorld.objects.get(id=5)
        self.assertEqual(obj.randomnumber, 50)

    def test_values_fetch_all(self):
        rows = list(FastFortune.objects.values("id", "message"))
        self.assertEqual(len(rows), 3)
        by_id = {r["id"]: r["message"] for r in rows}
        self.assertEqual(by_id[2], "alpha")
        self.assertEqual(by_id[3], "beta <x>")

    def test_values_list_fetch(self):
        rows = list(FastFortune.objects.values_list("id", "message"))
        self.assertEqual(len(rows), 3)

    def test_in_bulk(self):
        d = FastWorld.objects.in_bulk([1, 2, 3])
        self.assertEqual(set(d), {1, 2, 3})
        self.assertEqual(d[2].randomnumber, 20)

    def test_filter_update(self):
        n = FastWorld.objects.filter(pk=4).update(randomnumber=404)
        self.assertEqual(n, 1)
        self.assertEqual(FastWorld.objects.get(pk=4).randomnumber, 404)

    def test_model_save_update_fields(self):
        obj = FastWorld.objects.get(pk=6)
        obj.randomnumber = 606
        obj.save(update_fields=["randomnumber"])
        self.assertEqual(FastWorld.objects.get(pk=6).randomnumber, 606)

    def test_dataplane_get_and_update(self):
        # Both go through the C++ ORM data plane (no Python SQL string cache).
        row = FastWorld.objects.values_list("id", "randomnumber").get(id=1)
        self.assertEqual(row[0], 1)
        n = FastWorld.objects.filter(pk=1).update(randomnumber=11)
        self.assertEqual(n, 1)
        self.assertEqual(FastWorld.objects.get(pk=1).randomnumber, 11)

    def test_annotated_get_projects_attrs(self):
        from django.db.models import Value

        # Annotation aliases must appear as attributes on model instances
        # (native materialize projects SELECT annotation columns).
        qs = FastWorld.objects.annotate(x=Value(1))
        self.assertEqual(qs.get(pk=1).x, 1)

    def test_fortune_page_from_queryset(self):
        from django import native

        fortunes = list(FastFortune.objects.values("id", "message"))
        fortunes.append(
            {"id": 0, "message": "Additional fortune added at request time."}
        )
        fortunes.sort(key=lambda r: r["message"])
        html = native.render_fortune_page((r["id"], r["message"]) for r in fortunes)
        self.assertIn("Additional fortune added at request time.", html)
        self.assertIn("beta &lt;x&gt;", html)
        # Order: Additional..., alpha, beta, gamma
        pos_add = html.index("Additional")
        pos_alpha = html.index("alpha")
        pos_beta = html.index("beta")
        pos_gamma = html.index("gamma")
        self.assertLess(pos_add, pos_alpha)
        self.assertLess(pos_alpha, pos_beta)
        self.assertLess(pos_beta, pos_gamma)


@isolate_apps("native_orm_fastpath")
@modify_settings(INSTALLED_APPS={"append": "native_orm_fastpath"})
class OrmFastPathNativeOffTests(TestCase):
    """Same shapes must work with DJANGO_NATIVE=0 (pure Python dual path)."""

    @classmethod
    def setUpTestData(cls):
        FastWorld.objects.bulk_create(
            [FastWorld(id=i, randomnumber=i) for i in range(1, 6)]
        )

    def test_get_and_update_with_native_disabled(self):
        with mock.patch("django.native.AVAILABLE", False):
            with mock.patch("django.native._loader.AVAILABLE", False):
                row = FastWorld.objects.values_list("id", "randomnumber").get(id=2)
                self.assertEqual(row, (2, 2))
                n = FastWorld.objects.filter(pk=2).update(randomnumber=99)
                self.assertEqual(n, 1)
                obj = FastWorld.objects.get(pk=2)
                self.assertEqual(obj.randomnumber, 99)

