"""Tests for the C++ ORM data plane (schema + QuerySet + compile)."""

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
                ("id", "id", "id", "AutoField", True, False),
                (
                    "randomnumber",
                    "randomnumber",
                    "randomnumber",
                    "IntegerField",
                    False,
                    False,
                ),
            ],
        )
        self.assertIsInstance(mid, int)
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

    def test_filter_in_and_and(self):
        from django import _native

        orm = _native.orm
        mid = orm.register_model(
            "test.Item",
            "item",
            [
                ("id", "id", "id", "AutoField", True, False),
                ("name", "name", "name", "CharField", False, False),
            ],
        )
        qs = orm.QuerySet.create(mid, orm.DIALECT_SQLITE)
        self.assertTrue(qs.values_list(["id"], False))
        self.assertTrue(qs.filter_eq("name", "a"))
        self.assertTrue(qs.filter_in("id", [1, 2, 3]))
        sql, params = qs.compile_sql()
        self.assertIn("WHERE", sql)
        self.assertIn(" AND ", sql)
        self.assertIn("IN (%s, %s, %s)", sql)
        self.assertEqual(list(params), ["a", 1, 2, 3])

    def test_mysql_quoting(self):
        from django import _native

        orm = _native.orm
        mid = orm.register_model(
            "test.T",
            "t",
            [("id", "id", "id", "AutoField", True, False)],
        )
        qs = orm.QuerySet.create(mid, orm.DIALECT_MYSQL)
        qs.filter_eq("pk", 1)
        qs.set_limit(1)
        sql, params = qs.compile_sql()
        self.assertIn("`t`", sql)
        self.assertEqual(list(params), [1])

    def test_python_facade_export(self):
        from django.native import orm

        class Widget(models.Model):
            name = models.CharField(max_length=10)

            class Meta:
                app_label = "native_orm_dataplane_unit"

        mid = orm.export_model(Widget)
        self.assertIsNotNone(mid)
        self.assertEqual(orm.model_id("native_orm_dataplane_unit.Widget"), mid)


@isolate_apps("native_orm_dataplane")
class OrmDataPlaneFacadeTests(SimpleTestCase):
    def test_compile_values_list_get_from_model(self):
        from django.native import orm

        class World(models.Model):
            randomnumber = models.IntegerField()

            class Meta:
                app_label = "native_orm_dataplane"

        # Fake connection.vendor without a live DB round-trip.
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
        self.assertIn('FROM "native_orm_dataplane_world"', sql)
        self.assertIn('"randomnumber"', sql)
        self.assertEqual(params, [2])
        # Re-export must not collapse field ids (regression).
        compiled2 = orm.compile_values_list_get(
            World,
            field_names=["id", "randomnumber"],
            lookup_field="id",
            lookup_value=9,
            limit=21,
            connection=_Conn(),
        )
        self.assertIn('"randomnumber"', compiled2[0])
        self.assertNotEqual(
            compiled2[0].count('"id"'),
            # id appears as column + maybe AS — randomnumber must still be present
            0,
        )
