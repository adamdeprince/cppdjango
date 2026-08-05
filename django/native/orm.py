"""
Python facade for the native ORM data plane.

Steady-state query build + SQL compile live in C++ (django._native.orm).
This module: schema export from Django models, and thin helpers for execute.
"""

from __future__ import annotations

from typing import Any

from django.native._loader import AVAILABLE, get_native_module

__all__ = [
    "AVAILABLE",
    "clear_schema",
    "compile_values_list_get",
    "export_model",
    "model_id",
    "register_model_from_meta",
]


def _orm():
    impl = get_native_module()
    if impl is None:
        return None
    return getattr(impl, "orm", None)


def clear_schema() -> None:
    orm = _orm()
    if orm is not None:
        orm.clear_schema()


def model_id(label: str) -> int | None:
    orm = _orm()
    if orm is None:
        return None
    return orm.model_id(label)


def register_model_from_meta(model) -> int | None:
    """
    Snapshot model._meta into the C++ SchemaRegistry.

    Returns model_id or None if native ORM is unavailable.
    """
    orm = _orm()
    if orm is None:
        return None
    opts = model._meta
    label = f"{opts.app_label}.{opts.object_name}"
    fields = []
    for f in opts.concrete_fields:
        if not f.column:
            continue
        fields.append(
            (
                f.name,
                f.attname,
                f.column,
                f.__class__.__name__,
                bool(f.primary_key),
                bool(f.null),
            )
        )
    return orm.register_model(label, opts.db_table, fields)


def export_model(model) -> int | None:
    """Alias for register_model_from_meta."""
    return register_model_from_meta(model)


def dialect_for_connection(connection) -> int | None:
    orm = _orm()
    if orm is None:
        return None
    return int(orm.dialect_from_vendor(connection.vendor))


def compile_values_list_get(
    model,
    *,
    field_names: list[str],
    lookup_field: str,
    lookup_value: Any,
    limit: int,
    connection,
) -> tuple[str, list] | None:
    """
    Build TE-shaped::

        Model.objects.values_list(*field_names).get(lookup_field=lookup_value)

    entirely in the C++ data plane. Returns (sql, params) or None on miss.
    """
    orm = _orm()
    if orm is None:
        return None
    mid = register_model_from_meta(model)
    if mid is None:
        return None
    dialect = dialect_for_connection(connection)
    if dialect is None:
        return None
    qs = orm.QuerySet.create(int(mid), int(dialect))
    if not qs.values_list(list(field_names), False):
        return None
    if not qs.filter_eq(lookup_field, lookup_value):
        return None
    qs.set_limit(int(limit))
    sql, params = qs.compile_sql()
    if not sql:
        return None
    return sql, list(params)
