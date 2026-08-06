"""Tests for the C++ ORM data plane (schema + QuerySet + compile + DML)."""

from django.db import models
from django.test import SimpleTestCase
from django.test.utils import isolate_apps


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
                ("id", "id", "id", "AutoField", True, False, "", "", ""),
                ("name", "name", "name", "CharField", False, False, "", "", ""),
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
