"""
Python facade for the native ORM data plane.

Query build + SQL compile live in C++. This module exports schema from Meta,
bridges kwargs, and runs execute/materialize in few crossings.
"""

from __future__ import annotations

from typing import Any

from django.native._loader import AVAILABLE, get_native_module

__all__ = [
    "AVAILABLE",
    "build_queryset",
    "clear_schema",
    "compile_delete",
    "compile_select",
    "compile_update",
    "compile_values_list_get",
    "execute_fetchall",
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
    """Snapshot model._meta into the C++ SchemaRegistry (incl. simple FKs)."""
    orm = _orm()
    if orm is None:
        return None
    opts = model._meta
    label = f"{opts.app_label}.{opts.object_name}"
    fields = []
    for f in opts.concrete_fields:
        if not f.column:
            continue
        remote_table = ""
        remote_pk = ""
        remote_label = ""
        if f.is_relation and (f.many_to_one or f.one_to_one):
            try:
                remote = f.remote_field.model._meta
                remote_table = remote.db_table
                remote_pk = remote.pk.column
                remote_label = f"{remote.app_label}.{remote.object_name}"
            except Exception:
                remote_table = ""
        fields.append(
            (
                f.name,
                f.attname,
                f.column,
                f.__class__.__name__,
                bool(f.primary_key),
                bool(f.null),
                remote_table,
                remote_pk,
                remote_label,
            )
        )
    return orm.register_model(label, opts.db_table, fields)


def export_model(model) -> int | None:
    return register_model_from_meta(model)


def dialect_for_connection(connection) -> int | None:
    orm = _orm()
    if orm is None:
        return None
    return int(orm.dialect_from_vendor(connection.vendor))


def build_queryset(model, connection, *, kwargs=None, disjunctive=False):
    """
    Create a C++ QuerySet for model, optionally applying filter kwargs in one hop.
    Returns native QuerySet handle or None.
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
    if kwargs:
        # Normalize IN lists; leave scalars as-is for C++.
        payload = {}
        for k, v in kwargs.items():
            if not isinstance(k, str):
                return None
            payload[k] = v
        if not qs.filter_kwargs(payload, bool(disjunctive)):
            return None
    return qs


def compile_values_list_get(
    model,
    *,
    field_names: list[str],
    lookup_field: str,
    lookup_value: Any,
    limit: int,
    connection,
) -> tuple[str, list] | None:
    qs = build_queryset(model, connection, kwargs={lookup_field: lookup_value})
    if qs is None:
        return None
    if field_names and not qs.values_list(list(field_names), False):
        return None
    qs.set_limit(int(limit))
    sql, params = qs.compile_sql()
    if not sql:
        return None
    return sql, list(params)


def compile_select(
    model,
    connection,
    *,
    field_names: list[str] | None = None,
    kwargs: dict | None = None,
    limit: int | None = None,
    flat: bool = False,
) -> tuple[str, list] | None:
    qs = build_queryset(model, connection, kwargs=kwargs or {})
    if qs is None:
        return None
    if field_names:
        if not qs.values_list(list(field_names), flat):
            return None
    if limit is not None:
        qs.set_limit(int(limit))
    sql, params = qs.compile_sql()
    if not sql:
        return None
    return sql, list(params)


def compile_update(
    model,
    connection,
    *,
    filter_kwargs: dict,
    update_kwargs: dict,
) -> tuple[str, list] | None:
    qs = build_queryset(model, connection, kwargs=filter_kwargs)
    if qs is None or not update_kwargs:
        return None
    for name, value in update_kwargs.items():
        if not qs.add_update(name, value):
            return None
    sql, params = qs.compile_sql()
    if not sql:
        return None
    return sql, list(params)


def compile_delete(
    model, connection, *, filter_kwargs: dict
) -> tuple[str, list] | None:
    qs = build_queryset(model, connection, kwargs=filter_kwargs)
    if qs is None:
        return None
    qs.set_delete()
    sql, params = qs.compile_sql()
    if not sql:
        return None
    return sql, list(params)


def execute_fetchall(connection, sql: str, params=None) -> list:
    """
    Single execute + fetchall (materialize all rows in one DB round-trip).
    Result arena at the Python/DBAPI boundary: one list of tuples out.
    """
    with connection.cursor() as cursor:
        cursor.execute(sql, params or ())
        return cursor.fetchall()


def execute_fetchone_pair(connection, sql: str, params=None):
    """Fetch up to 2 rows to detect MultipleObjectsReturned for get()."""
    with connection.cursor() as cursor:
        cursor.execute(sql, params or ())
        row = cursor.fetchone()
        if row is None:
            return None, False
        extra = cursor.fetchone() is not None
        return row, extra
