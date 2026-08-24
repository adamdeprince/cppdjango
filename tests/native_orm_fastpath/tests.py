"""
Correctness harness for Tier-1+ ORM fast paths (native on and DJANGO_NATIVE=0).

Covers TE-shaped patterns: values_list.get, model get, values fetch, in_bulk,
filter().update, and fortune HTML rendering.
"""

import os
from unittest import mock

from django.test import SimpleTestCase, TestCase, modify_settings
from django.test.utils import isolate_apps

from .models import FastArticle, FastAuthor, FastFortune, FastWorld


@isolate_apps("native_orm_fastpath")
class SimpleSQLHelpersTests(SimpleTestCase):
    def test_simple_update_eq_sql_shape(self):
        from django import native

        sql = native.simple_update_eq_sql('"world"', ['"randomnumber"'], '"id"')
        self.assertEqual(sql, 'UPDATE "world" SET "randomnumber" = %s WHERE "id" = %s')

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

    def test_filtered_values_list_preserves_ordering(self):
        rows = list(
            FastWorld.objects.filter(id__in=[1, 3, 2])
            .order_by("-id")
            .values_list("id", flat=True)
        )
        self.assertEqual(rows, [3, 2, 1])

    def test_exact_filtered_values_list(self):
        rows = list(
            FastWorld.objects.filter(id=8).values_list("id", "randomnumber")
        )
        self.assertEqual(rows, [(8, 80)])

    def test_deferred_filter_then_second_filter_preserves_both_predicates(self):
        rows = list(
            FastWorld.objects.filter(id__in=[1, 2, 3])
            .filter(randomnumber=20)
            .values_list("id", "randomnumber")
        )
        self.assertEqual(rows, [(2, 20)])

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

    def test_native_primitive_terminals_skip_python_field_prep(self):
        from django import native
        from django.native import orm

        if not native.AVAILABLE:
            self.skipTest("native extension required")
        # Export before installing spies so schema setup itself isn't part of
        # the terminal assertion.
        orm.register_model_from_meta(FastWorld)
        id_field = FastWorld._meta.get_field("id")
        value_field = FastWorld._meta.get_field("randomnumber")
        with (
            mock.patch.object(
                id_field,
                "get_db_prep_value",
                side_effect=AssertionError("point lookup used Python prep"),
            ) as lookup_prep,
            mock.patch.object(
                value_field,
                "get_db_prep_save",
                side_effect=AssertionError("update used Python prep"),
            ) as update_prep,
        ):
            self.assertEqual(
                FastWorld.objects.values_list("id", "randomnumber").get(id=2),
                (2, 20),
            )
            self.assertEqual(
                FastWorld.objects.filter(id=2).update(randomnumber=202), 1
            )
        lookup_prep.assert_not_called()
        update_prep.assert_not_called()
        self.assertEqual(FastWorld.objects.get(id=2).randomnumber, 202)

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


@isolate_apps("native_orm_fastpath")
@modify_settings(INSTALLED_APPS={"append": "native_orm_fastpath"})
class FetchModeNativeTests(TestCase):
    """Django 6.1 fetch modes through native materialize / get / peers."""

    @classmethod
    def setUpTestData(cls):
        a1 = FastAuthor.objects.create(id=1, name="Ada")
        a2 = FastAuthor.objects.create(id=2, name="Grace")
        FastArticle.objects.bulk_create(
            [
                FastArticle(id=1, title="one", author=a1),
                FastArticle(id=2, title="two", author=a1),
                FastArticle(id=3, title="three", author=a2),
            ]
        )

    def test_fetch_peers_shared_on_list(self):
        from django.db.models import FETCH_PEERS

        articles = list(FastArticle.objects.fetch_mode(FETCH_PEERS).order_by("id"))
        self.assertEqual(len(articles), 3)
        for a in articles:
            self.assertIs(a._state.fetch_mode, FETCH_PEERS)
            self.assertIs(a._state.peers, articles[0]._state.peers)
        # Peer weakrefs resolve to the same instances.
        live = [ref() for ref in articles[0]._state.peers]
        self.assertEqual(live, articles)

    def test_fetch_peers_forward_fk_batches(self):
        from django.db.models import FETCH_PEERS

        a1, a2 = FastArticle.objects.fetch_mode(FETCH_PEERS).filter(
            author_id=1
        ).order_by("id")
        # Accessing author on either peer should use fetch_many (one query).
        with self.assertNumQueries(1):
            self.assertEqual(a1.author.name, "Ada")
            self.assertEqual(a2.author.name, "Ada")
        self.assertIs(a1.author._state.fetch_mode, FETCH_PEERS)

    def test_fetch_raise_blocks_fk(self):
        from django.core.exceptions import FieldFetchBlocked
        from django.db.models import FETCH_RAISE

        article = FastArticle.objects.fetch_mode(FETCH_RAISE).get(pk=1)
        self.assertIs(article._state.fetch_mode, FETCH_RAISE)
        with self.assertRaises(FieldFetchBlocked):
            _ = article.author

    def test_select_related_copies_fetch_mode(self):
        from django.db.models import FETCH_PEERS

        article = (
            FastArticle.objects.fetch_mode(FETCH_PEERS)
            .select_related("author")
            .get(pk=1)
        )
        self.assertIs(article._state.fetch_mode, FETCH_PEERS)
        self.assertIs(article.author._state.fetch_mode, FETCH_PEERS)

    def test_in_bulk_propagates_fetch_mode(self):
        from django.db.models import FETCH_PEERS

        bulk = FastArticle.objects.fetch_mode(FETCH_PEERS).in_bulk([1, 2])
        self.assertEqual(set(bulk), {1, 2})
        for obj in bulk.values():
            self.assertIs(obj._state.fetch_mode, FETCH_PEERS)

    def test_create_propagates_fetch_mode(self):
        from django.db.models import FETCH_PEERS

        author = FastAuthor.objects.fetch_mode(FETCH_PEERS).create(name="Ken")
        self.assertIs(author._state.fetch_mode, FETCH_PEERS)

    def test_iterator_shares_peers_within_chunk(self):
        from django.db.models import FETCH_PEERS

        # iterator() uses ModelIterable (after any native→Python materialize),
        # which must still attach a shared peer list for FETCH_PEERS.
        it = FastArticle.objects.fetch_mode(FETCH_PEERS).order_by("id").iterator(
            chunk_size=10
        )
        articles = list(it)
        self.assertEqual(len(articles), 3)
        for a in articles:
            self.assertIs(a._state.fetch_mode, FETCH_PEERS)
            self.assertIs(a._state.peers, articles[0]._state.peers)

    def test_iterator_prefetch_chunk_preserves_mode(self):
        from django.db.models import FETCH_PEERS

        qs = (
            FastArticle.objects.fetch_mode(FETCH_PEERS)
            .order_by("id")
            .prefetch_related("author")
        )
        articles = list(qs.iterator(chunk_size=2))
        self.assertEqual(len(articles), 3)
        for a in articles:
            self.assertIs(a._state.fetch_mode, FETCH_PEERS)
            # Prefetch ran per chunk; author is cached with same mode.
            self.assertIs(a.author._state.fetch_mode, FETCH_PEERS)

    def test_reverse_fk_copies_fetch_mode(self):
        from django.db.models import FETCH_PEERS

        author = FastAuthor.objects.fetch_mode(FETCH_PEERS).get(pk=1)
        self.assertIs(author._state.fetch_mode, FETCH_PEERS)
        # Reverse manager inherits the parent's fetch_mode onto related rows.
        articles = list(author.articles.order_by("id"))
        self.assertEqual(len(articles), 2)
        for a in articles:
            self.assertIs(a._state.fetch_mode, FETCH_PEERS)

    def test_prefetch_related_copies_fetch_mode(self):
        from django.db.models import FETCH_PEERS

        articles = list(
            FastArticle.objects.fetch_mode(FETCH_PEERS)
            .order_by("id")
            .prefetch_related("author")
        )
        self.assertIs(articles[0]._state.fetch_mode, FETCH_PEERS)
        self.assertIs(articles[0].author._state.fetch_mode, FETCH_PEERS)
        # Peer-fetched authors for Ada's two articles should share identity.
        self.assertIs(articles[0].author, articles[1].author)

    def test_raw_clone_preserves_fetch_mode(self):
        from django.db.models import FETCH_PEERS, FETCH_RAISE

        raw = FastArticle.objects.fetch_mode(FETCH_PEERS).raw(
            "SELECT * FROM native_orm_fastpath_fastarticle ORDER BY id"
        )
        # prefetch_related clones the RawQuerySet — mode must survive.
        cloned = raw.prefetch_related()
        articles = list(cloned)
        self.assertEqual(len(articles), 3)
        for a in articles:
            self.assertIs(a._state.fetch_mode, FETCH_PEERS)
            self.assertIs(a._state.peers, articles[0]._state.peers)

        raised = FastArticle.objects.raw(
            "SELECT * FROM native_orm_fastpath_fastarticle WHERE id = %s", [1]
        ).fetch_mode(FETCH_RAISE)
        obj = list(raised)[0]
        self.assertIs(obj._state.fetch_mode, FETCH_RAISE)

    def test_get_or_create_propagates_fetch_mode(self):
        from django.db.models import FETCH_PEERS

        author, created = FastAuthor.objects.fetch_mode(FETCH_PEERS).get_or_create(
            name="New", defaults={}
        )
        self.assertTrue(created)
        self.assertIs(author._state.fetch_mode, FETCH_PEERS)
        author2, created2 = FastAuthor.objects.fetch_mode(FETCH_PEERS).get_or_create(
            name="New"
        )
        self.assertFalse(created2)
        self.assertIs(author2._state.fetch_mode, FETCH_PEERS)

    def test_pickle_roundtrip_fetch_modes(self):
        import pickle

        from django.db.models import FETCH_ONE, FETCH_PEERS, FETCH_RAISE

        for mode in (FETCH_ONE, FETCH_PEERS, FETCH_RAISE):
            with self.subTest(mode=mode):
                objs = list(FastArticle.objects.fetch_mode(mode).order_by("id")[:1])
                restored = pickle.loads(pickle.dumps(objs))
                self.assertIs(restored[0]._state.fetch_mode, mode)

    def test_native_off_fetch_peers_still_works(self):
        from django.db.models import FETCH_PEERS

        with mock.patch("django.native.AVAILABLE", False):
            with mock.patch("django.native._loader.AVAILABLE", False):
                articles = list(
                    FastArticle.objects.fetch_mode(FETCH_PEERS).order_by("id")
                )
                self.assertEqual(len(articles), 3)
                for a in articles:
                    self.assertIs(a._state.fetch_mode, FETCH_PEERS)
                    self.assertIs(a._state.peers, articles[0]._state.peers)
                with self.assertNumQueries(1):
                    self.assertEqual(articles[0].author.name, "Ada")
                    self.assertEqual(articles[1].author.name, "Ada")
