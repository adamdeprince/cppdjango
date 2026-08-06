"""Tests for the C++ ORM data plane (schema + QuerySet + compile + DML)."""

from django.db import models
from django.test import SimpleTestCase
from django.test.utils import isolate_apps


def _row(*args):
    """Pad field descriptors to the 14-tuple register_model format."""
    row = list(args) + [""] * 14
    return tuple(row[:14])


class OrmDataPlaneUnitTests(SimpleTestCase):
    def setUp(self):
        from django import native

        if not native.AVAILABLE:
            self.skipTest("native extension required")
        from django.native import orm

        orm.clear_schema()

    def test_register_and_compile_eq(self):
        from django import _native

        orm = _native.orm
        mid = orm.register_model(
            "test.World",
            "world",
            [
                ("id", "id", "id", "AutoField", True, False, "", "", ""),
                (
                    "randomnumber",
                    "randomnumber",
                    "randomnumber",
                    "IntegerField",
                    False,
                    False,
                    "",
                    "",
                    "",
                ),
            ],
        )
        qs = orm.QuerySet.create(mid, orm.DIALECT_POSTGRES)
        self.assertTrue(qs.values_list(["id", "randomnumber"], False))
        self.assertTrue(qs.filter_eq("id", 7))
        qs.set_limit(21)
        sql, params = qs.compile_sql()
        self.assertEqual(
            sql,
            'SELECT "world"."id" AS "id", "world"."randomnumber" AS "randomnumber" '
            'FROM "world" WHERE "world"."id" = %s LIMIT 21',
        )
        self.assertEqual(list(params), [7])

    def test_filter_kwargs_gt_and_in(self):
        from django import _native

        orm = _native.orm
        mid = orm.register_model(
            "test.Item",
            "item",
            [
                ("id", "id", "id", "AutoField", True, False, "", "", ""),
                ("score", "score", "score", "IntegerField", False, False, "", "", ""),
            ],
        )
        qs = orm.QuerySet.create(mid, orm.DIALECT_SQLITE)
        self.assertTrue(
            qs.filter_kwargs({"score__gt": 10, "id__in": [1, 2, 3]}, False)
        )
        sql, params = qs.compile_sql()
        self.assertIn(">", sql)
        self.assertIn("IN (%s, %s, %s)", sql)
        self.assertEqual(list(params), [10, 1, 2, 3])

    def test_filter_isnull(self):
        from django import _native

        orm = _native.orm
        mid = orm.register_model(
            "test.N",
            "n",
            [
                ("id", "id", "id", "AutoField", True, False, "", "", ""),
                ("name", "name", "name", "CharField", False, True, "", "", ""),
            ],
        )
        qs = orm.QuerySet.create(mid, orm.DIALECT_POSTGRES)
        self.assertTrue(qs.filter_isnull("name", True))
        sql, params = qs.compile_sql()
        self.assertIn("IS NULL", sql)
        self.assertEqual(list(params), [])

    def test_join_fk_path(self):
        from django import _native

        orm = _native.orm
        orm.register_model(
            "test.Author",
            "author",
            [
                _row("id", "id", "id", "AutoField", True, False),
                _row("name", "name", "name", "CharField", False, False),
            ],
        )
        mid = orm.register_model(
            "test.Book",
            "book",
            [
                _row("id", "id", "id", "AutoField", True, False),
                _row(
                    "author",
                    "author_id",
                    "author_id",
                    "ForeignKey",
                    False,
                    False,
                    "author",
                    "id",
                    "test.Author",
                    "fk",
                ),
            ],
        )
        qs = orm.QuerySet.create(mid, orm.DIALECT_POSTGRES)
        self.assertTrue(qs.filter_kwargs({"author__name": "Ada"}, False))
        sql, params = qs.compile_sql()
        self.assertIn("INNER JOIN", sql)
        self.assertIn('"author"', sql)
        self.assertEqual(list(params), ["Ada"])

    def test_multi_hop_joins(self):
        from django import _native

        orm = _native.orm
        orm.register_model(
            "test.Country",
            "country",
            [
                ("id", "id", "id", "AutoField", True, False, "", "", ""),
                ("code", "code", "code", "CharField", False, False, "", "", ""),
            ],
        )
        orm.register_model(
            "test.Author",
            "author",
            [
                ("id", "id", "id", "AutoField", True, False, "", "", ""),
                (
                    "country",
                    "country_id",
                    "country_id",
                    "ForeignKey",
                    False,
                    False,
                    "country",
                    "id",
                    "test.Country",
                ),
            ],
        )
        mid = orm.register_model(
            "test.Book",
            "book",
            [
                ("id", "id", "id", "AutoField", True, False, "", "", ""),
                (
                    "author",
                    "author_id",
                    "author_id",
                    "ForeignKey",
                    False,
                    False,
                    "author",
                    "id",
                    "test.Author",
                ),
            ],
        )
        qs = orm.QuerySet.create(mid, orm.DIALECT_POSTGRES)
        self.assertTrue(qs.filter_kwargs({"author__country__code": "US"}, False))
        sql, params = qs.compile_sql()
        # Two joins
        self.assertEqual(sql.count("INNER JOIN"), 2)
        self.assertIn('"country"', sql)
        self.assertIn('"code"', sql)
        self.assertEqual(list(params), ["US"])

    def test_apply_q_tree(self):
        from django import _native

        orm = _native.orm
        mid = orm.register_model(
            "test.Item",
            "item",
            [
                ("id", "id", "id", "AutoField", True, False, "", "", ""),
                ("score", "score", "score", "IntegerField", False, False, "", "", ""),
                ("name", "name", "name", "CharField", False, False, "", "", ""),
            ],
        )
        qs = orm.QuerySet.create(mid, orm.DIALECT_POSTGRES)
        tree = {
            "kind": "or",
            "children": [
                {"kind": "atom", "key": "score__gte", "values": [10]},
                {
                    "kind": "and",
                    "children": [
                        {"kind": "atom", "key": "name", "values": ["x"]},
                        {"kind": "atom", "key": "id__in", "values": [1, 2]},
                    ],
                },
            ],
        }
        self.assertTrue(qs.apply_q(tree))
        sql, params = qs.compile_sql()
        self.assertIn(" OR ", sql)
        self.assertIn(" AND ", sql)
        self.assertEqual(list(params), [10, "x", 1, 2])

    def test_apply_q_not(self):
        from django import _native

        orm = _native.orm
        mid = orm.register_model(
            "test.N2",
            "n2",
            [
                ("id", "id", "id", "AutoField", True, False, "", "", ""),
                ("name", "name", "name", "CharField", False, False, "", "", ""),
            ],
        )
        qs = orm.QuerySet.create(mid, orm.DIALECT_SQLITE)
        tree = {
            "kind": "not",
            "children": [{"kind": "atom", "key": "name", "values": ["nope"]}],
        }
        self.assertTrue(qs.apply_q(tree))
        sql, params = qs.compile_sql()
        self.assertIn("NOT (", sql)
        self.assertEqual(list(params), ["nope"])

    def test_update_and_delete_compile(self):
        from django import _native

        orm = _native.orm
        mid = orm.register_model(
            "test.U",
            "u",
            [
                ("id", "id", "id", "AutoField", True, False, "", "", ""),
                ("n", "n", "n", "IntegerField", False, False, "", "", ""),
            ],
        )
        qs = orm.QuerySet.create(mid, orm.DIALECT_POSTGRES)
        qs.filter_eq("id", 1)
        qs.add_update("n", 99)
        sql, params = qs.compile_sql()
        self.assertTrue(sql.startswith("UPDATE"))
        self.assertIn("SET", sql)
        self.assertEqual(list(params), [99, 1])

        qs2 = orm.QuerySet.create(mid, orm.DIALECT_POSTGRES)
        qs2.filter_eq("id", 2)
        qs2.set_delete()
        sql2, params2 = qs2.compile_sql()
        self.assertTrue(sql2.startswith("DELETE FROM"))
        self.assertEqual(list(params2), [2])

    def test_mysql_quoting(self):
        from django import _native

        orm = _native.orm
        mid = orm.register_model(
            "test.T",
            "t",
            [("id", "id", "id", "AutoField", True, False, "", "", "")],
        )
        qs = orm.QuerySet.create(mid, orm.DIALECT_MYSQL)
        qs.filter_eq("pk", 1)
        qs.set_limit(1)
        sql, params = qs.compile_sql()
        self.assertIn("`t`", sql)
        self.assertEqual(list(params), [1])


@isolate_apps("native_orm_dataplane")
class OrmDataPlaneFacadeTests(SimpleTestCase):
    def test_compile_values_list_get_from_model(self):
        from django.native import orm

        class World(models.Model):
            randomnumber = models.IntegerField()

            class Meta:
                app_label = "native_orm_dataplane"

        class _Conn:
            vendor = "sqlite"

        compiled = orm.compile_values_list_get(
            World,
            field_names=["id", "randomnumber"],
            lookup_field="id",
            lookup_value=2,
            limit=21,
            connection=_Conn(),
        )
        self.assertIsNotNone(compiled)
        sql, params = compiled
        self.assertIn("randomnumber", sql)
        self.assertEqual(params, [2])

        compiled2 = orm.compile_update(
            World,
            _Conn(),
            filter_kwargs={"id": 2},
            update_kwargs={"randomnumber": 5},
        )
        self.assertIsNotNone(compiled2)
        self.assertTrue(compiled2[0].startswith("UPDATE"))
        self.assertEqual(compiled2[1], [5, 2])

    def test_q_to_tree_and_compile_select(self):
        from django.db.models import Q
        from django.native import orm

        class Country(models.Model):
            code = models.CharField(max_length=2)

            class Meta:
                app_label = "native_orm_dataplane"

        class Author(models.Model):
            country = models.ForeignKey(Country, on_delete=models.CASCADE)
            name = models.CharField(max_length=40)

            class Meta:
                app_label = "native_orm_dataplane"

        class Book(models.Model):
            author = models.ForeignKey(Author, on_delete=models.CASCADE)
            title = models.CharField(max_length=40)

            class Meta:
                app_label = "native_orm_dataplane"

        class _Conn:
            vendor = "postgresql"

        q = Q(title="X") | Q(author__country__code="US", author__name="Ada")
        tree = orm.q_to_tree(q)
        self.assertIsNotNone(tree)
        self.assertEqual(tree["kind"], "or")

        compiled = orm.compile_select(
            Book,
            _Conn(),
            field_names=["id", "title"],
            q=q,
        )
        self.assertIsNotNone(compiled)
        sql, params = compiled
        self.assertEqual(sql.count("INNER JOIN"), 2)
        self.assertIn(" OR ", sql)
        self.assertIn("Ada", params)
        self.assertIn("US", params)

    def test_q_xor(self):
        from django.db.models import Q
        from django.native import orm

        class Item(models.Model):
            a = models.IntegerField()
            b = models.IntegerField()

            class Meta:
                app_label = "native_orm_dataplane"

        class _Conn:
            vendor = "sqlite"

        q = Q(a=1) ^ Q(b=2)
        tree = orm.q_to_tree(q)
        self.assertEqual(tree["kind"], "xor")
        compiled = orm.compile_select(Item, _Conn(), q=q)
        self.assertIsNotNone(compiled)
        sql, params = compiled
        self.assertIn("CASE WHEN", sql)
        self.assertIn(" OR ", sql)
        self.assertEqual(params.count(1) + params.count(2), len(params))

    def test_reverse_fk_and_m2m_joins(self):
        """Reverse FK and M2M join SQL via explicit schema rows."""
        from django import _native

        orm = _native.orm
        # Book model (remote for reverse FK / M2M target)
        orm.register_model(
            "rel.Book",
            "book",
            [
                _row("id", "id", "id", "AutoField", True, False),
                _row("title", "title", "title", "CharField", False, False),
                _row(
                    "author",
                    "author_id",
                    "author_id",
                    "ForeignKey",
                    False,
                    False,
                    "author",
                    "id",
                    "rel.Author",
                    "fk",
                ),
            ],
        )
        orm.register_model(
            "rel.Tag",
            "tag",
            [
                _row("id", "id", "id", "AutoField", True, False),
                _row("name", "name", "name", "CharField", False, False),
            ],
        )
        # Author with reverse FK accessor "books"
        author_id = orm.register_model(
            "rel.Author",
            "author",
            [
                _row("id", "id", "id", "AutoField", True, False),
                _row("name", "name", "name", "CharField", False, False),
                _row(
                    "books",
                    "books",
                    "",
                    "ManyToOneRel",
                    False,
                    True,
                    "book",
                    "id",
                    "rel.Book",
                    "rev_fk",
                    "",
                    "",
                    "",
                    "author_id",
                ),
            ],
        )
        # Book with M2M tags (re-register with m2m field)
        book_id = orm.register_model(
            "rel.Book",
            "book",
            [
                _row("id", "id", "id", "AutoField", True, False),
                _row("title", "title", "title", "CharField", False, False),
                _row(
                    "author",
                    "author_id",
                    "author_id",
                    "ForeignKey",
                    False,
                    False,
                    "author",
                    "id",
                    "rel.Author",
                    "fk",
                ),
                _row(
                    "tags",
                    "tags",
                    "",
                    "ManyToManyField",
                    False,
                    True,
                    "tag",
                    "id",
                    "rel.Tag",
                    "m2m",
                    "book_tags",
                    "book_id",
                    "tag_id",
                    "",
                ),
            ],
        )
        qs = orm.QuerySet.create(author_id, orm.DIALECT_POSTGRES)
        self.assertTrue(qs.filter_kwargs({"books__title": "Dune"}, False))
        sql, params = qs.compile_sql()
        self.assertIn("INNER JOIN", sql)
        self.assertIn('"book"', sql)
        self.assertIn("author_id", sql)
        self.assertEqual(list(params), ["Dune"])

        qs2 = orm.QuerySet.create(book_id, orm.DIALECT_POSTGRES)
        self.assertTrue(qs2.filter_kwargs({"tags__name": "scifi"}, False))
        sql2, params2 = qs2.compile_sql()
        self.assertGreaterEqual(sql2.count("INNER JOIN"), 2)
        self.assertIn("book_tags", sql2)
        self.assertEqual(list(params2), ["scifi"])

    def test_stock_filter_exclude_wires_native_handle(self):
        from django.db.models import Q
        from django.native import orm

        class World(models.Model):
            randomnumber = models.IntegerField()

            class Meta:
                app_label = "native_orm_dataplane_wire"

        class _Conn:
            vendor = "sqlite"

        # Simulate Manager.all().filter via QuerySet construction
        qs = World.objects.all()
        qs = qs.filter(randomnumber__gt=5)
        self.assertIsNotNone(getattr(qs, "_native_qs", None))
        self.assertFalse(getattr(qs, "_native_disabled", True))

        qs2 = qs.exclude(randomnumber=99)
        self.assertIsNotNone(qs2._native_qs)

        # values_list fetch uses native handle
        # (no DB rows required for compile path via handle)
        handle = qs2._native_handle_for_sql()
        self.assertIsNotNone(handle)
        h = handle.clone()
        self.assertTrue(h.values_list(["id", "randomnumber"], False))
        sql, params = h.compile_sql()
        self.assertIn(">", sql)
        self.assertIn("NOT", sql)

        qs3 = World.objects.filter(Q(randomnumber=1) ^ Q(randomnumber=2))
        self.assertIsNotNone(qs3._native_qs)

    def test_annotate_aggregate_and_subquery_compile(self):
        from django import _native

        orm = _native.orm
        mid = orm.register_model(
            "agg.T",
            "t",
            [
                _row("id", "id", "id", "AutoField", True, False),
                _row("n", "n", "n", "IntegerField", False, False),
            ],
        )
        qs = orm.QuerySet.create(mid, orm.DIALECT_POSTGRES)
        qs.clear_select()
        self.assertTrue(qs.annotate_aggregate("c", "COUNT", "", False, True))
        sql, params = qs.compile_sql()
        self.assertIn("COUNT(*)", sql)
        self.assertIn('AS "c"', sql)

        qs2 = orm.QuerySet.create(mid, orm.DIALECT_POSTGRES)
        qs2.filter_eq("id", 1)
        self.assertTrue(
            qs2.annotate_sql("s", "SELECT 1", [])
        )
        sql2, _ = qs2.compile_sql()
        self.assertIn("(SELECT 1)", sql2)

        qs3 = orm.QuerySet.create(mid, orm.DIALECT_SQLITE)
        self.assertTrue(
            qs3.filter_subquery("id", orm.OP_IN, "SELECT %s", [1])
        )
        sql3, p3 = qs3.compile_sql()
        self.assertIn("IN (SELECT %s)", sql3)
        self.assertEqual(list(p3), [1])

    def test_select_related_compile(self):
        from django import _native

        orm = _native.orm
        orm.register_model(
            "sr.Author",
            "author",
            [
                _row("id", "id", "id", "AutoField", True, False),
                _row("name", "name", "name", "CharField", False, False),
            ],
        )
        mid = orm.register_model(
            "sr.Book",
            "book",
            [
                _row("id", "id", "id", "AutoField", True, False),
                _row("title", "title", "title", "CharField", False, False),
                _row(
                    "author",
                    "author_id",
                    "author_id",
                    "ForeignKey",
                    False,
                    False,
                    "author",
                    "id",
                    "sr.Author",
                    "fk",
                ),
            ],
        )
        qs = orm.QuerySet.create(mid, orm.DIALECT_POSTGRES)
        self.assertTrue(qs.select_model_columns())
        self.assertTrue(qs.add_select_related("author"))
        sql, params = qs.compile_sql()
        self.assertIn("LEFT OUTER JOIN", sql)
        self.assertIn('"author"', sql)
        info = qs.related_selects_info()
        self.assertEqual(len(info), 1)
        self.assertEqual(info[0]["path"], "author")
        self.assertGreater(info[0]["count"], 0)

        # Unrestricted select_related (all FKs)
        qs2 = orm.QuerySet.create(mid, orm.DIALECT_POSTGRES)
        self.assertTrue(qs2.select_model_columns())
        self.assertTrue(qs2.add_select_related_all(2))
        sql2, _ = qs2.compile_sql()
        self.assertIn("LEFT OUTER JOIN", sql2)

    def test_case_when_and_native_subquery(self):
        from django import _native

        orm = _native.orm
        mid = orm.register_model(
            "cx.T",
            "t",
            [
                _row("id", "id", "id", "AutoField", True, False),
                _row("n", "n", "n", "IntegerField", False, False),
            ],
        )
        qs = orm.QuerySet.create(mid, orm.DIALECT_POSTGRES)
        qs.select_model_columns()
        when = {"kind": "atom", "key": "n__gt", "values": [10]}
        self.assertTrue(
            qs.annotate_case("bucket", [(when, "hi")], "lo")
        )
        sql, params = qs.compile_sql()
        self.assertIn("CASE", sql)
        self.assertIn("WHEN", sql)
        self.assertIn("THEN %s", sql)
        self.assertIn("ELSE %s", sql)

        # Nested QuerySet subquery (native compile)
        sub = orm.QuerySet.create(mid, orm.DIALECT_POSTGRES)
        sub.values_list(["id"], False)
        sub.filter_eq("n", 1)
        qs2 = orm.QuerySet.create(mid, orm.DIALECT_POSTGRES)
        self.assertTrue(qs2.filter_subquery_qs("id", orm.OP_IN, sub))
        sql2, p2 = qs2.compile_sql()
        self.assertIn("IN (", sql2)
        self.assertIn("SELECT", sql2)

        qs3 = orm.QuerySet.create(mid, orm.DIALECT_POSTGRES)
        qs3.select_model_columns()
        self.assertTrue(qs3.annotate_subquery_qs("sid", sub))
        sql3, _ = qs3.compile_sql()
        self.assertIn("AS \"sid\"", sql3)

    def test_prefetch_secondary_sql(self):
        from django import _native

        orm = _native.orm
        orm.register_model(
            "pf.Book",
            "book",
            [
                _row("id", "id", "id", "AutoField", True, False),
                _row("title", "title", "title", "CharField", False, False),
                _row(
                    "author",
                    "author_id",
                    "author_id",
                    "ForeignKey",
                    False,
                    False,
                    "author",
                    "id",
                    "pf.Author",
                    "fk",
                ),
            ],
        )
        author_id = orm.register_model(
            "pf.Author",
            "author",
            [
                _row("id", "id", "id", "AutoField", True, False),
                _row("name", "name", "name", "CharField", False, False),
                _row(
                    "books",
                    "books",
                    "",
                    "ManyToOneRel",
                    False,
                    True,
                    "book",
                    "id",
                    "pf.Book",
                    "rev_fk",
                    "",
                    "",
                    "",
                    "author_id",
                ),
            ],
        )
        qs = orm.QuerySet.create(author_id, orm.DIALECT_POSTGRES)
        self.assertTrue(qs.add_prefetch("books"))
        specs = qs.prefetch_specs()
        self.assertEqual(len(specs), 1)
        sql, params = qs.compile_prefetch_secondary(specs[0], [1, 2, 3])
        self.assertTrue(sql)
        self.assertIn("SELECT", sql)
        self.assertIn("IN (", sql)
        self.assertEqual(list(params), [1, 2, 3])
