"""
Python facade for the native ORM data plane.

Query build + SQL compile live in C++. This module exports schema from Meta,
bridges kwargs / Q trees, and runs execute/materialize in few crossings.
"""

from __future__ import annotations

from typing import Any

from django.native._loader import AVAILABLE, get_native_module

__all__ = [
    "AVAILABLE",
    "apply_q",
    "build_queryset",
    "clear_schema",
    "compile_delete",
    "compile_select",
    "compile_update",
    "compile_values_list_get",
    "execute_fetchall",
    "execute_fetchone_pair",
    "export_model",
    "model_id",
    "q_to_tree",
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


def register_model_from_meta(model, _seen: set | None = None) -> int | None:
    """
    Snapshot model._meta into the C++ SchemaRegistry.

    Recursively exports related models so multi-hop joins can resolve columns.
    """
    orm = _orm()
    if orm is None:
        return None
    if _seen is None:
        _seen = set()
    opts = model._meta
    label = f"{opts.app_label}.{opts.object_name}"
    if label in _seen:
        return model_id(label)
    _seen.add(label)

    fields = []
    for f in opts.concrete_fields:
        if not f.column:
            continue
        remote_table = ""
        remote_pk = ""
        remote_label = ""
        if f.is_relation and (f.many_to_one or f.one_to_one):
            try:
                remote_model = f.remote_field.model
                # Ensure remote schema exists for multi-hop.
                register_model_from_meta(remote_model, _seen)
                remote = remote_model._meta
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


def q_to_tree(node) -> dict | None:
    """
    Lower a Django Q (or (key, value) child) to a JSON-like tree for C++ apply_q.

    Returns None if the tree contains unsupported nodes (expressions, XOR, …).
    """
    from django.db.models import Q
    from django.db.models.lookups import Lookup

    if isinstance(node, Q):
        if node.connector == Q.XOR:
            return None  # not yet
        if node.negated:
            # NOT (children combined with connector)
            inner_connector = node.connector or Q.AND
            kind = "or" if inner_connector == Q.OR else "and"
            children = []
            for c in node.children:
                lc = q_to_tree(c)
                if lc is None:
                    return None
                children.append(lc)
            body = {"kind": kind, "children": children}
            return {"kind": "not", "children": [body]}
        kind = "or" if node.connector == Q.OR else "and"
        children = []
        for c in node.children:
            lc = q_to_tree(c)
            if lc is None:
                return None
            children.append(lc)
        return {"kind": kind, "children": children}

    # (lookup, value) pair from Q kwargs / children
    if isinstance(node, tuple) and len(node) == 2 and isinstance(node[0], str):
        key, val = node
        simple = (str, int, float, bool, type(None), bytes)
        if isinstance(val, (list, tuple)):
            for item in val:
                if not isinstance(item, simple):
                    return None
            values = list(val)
        elif isinstance(val, simple):
            values = [val]
        else:
            # F(), expressions, model instances without prep — miss
            return None
        return {"kind": "atom", "key": key, "values": values}

    if isinstance(node, Lookup):
        return None
    return None


def apply_q(qs, q_obj) -> bool:
    """Apply a Django Q to a native QuerySet handle. Returns False on miss."""
    tree = q_to_tree(q_obj)
    if tree is None:
        return False
    return bool(qs.apply_q(tree))


def build_queryset(
    model,
    connection,
    *,
    kwargs=None,
    disjunctive=False,
    q=None,
):
    """
    Create a C++ QuerySet for model, optionally applying kwargs and/or a Q tree.
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
    if q is not None:
        if not apply_q(qs, q):
            return None
    if kwargs:
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
    q=None,
    limit: int | None = None,
    flat: bool = False,
) -> tuple[str, list] | None:
    qs = build_queryset(model, connection, kwargs=kwargs or None, q=q)
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
    filter_kwargs: dict | None = None,
    update_kwargs: dict,
    q=None,
) -> tuple[str, list] | None:
    qs = build_queryset(
        model, connection, kwargs=filter_kwargs or None, q=q
    )
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
    model,
    connection,
    *,
    filter_kwargs: dict | None = None,
    q=None,
) -> tuple[str, list] | None:
    qs = build_queryset(
        model, connection, kwargs=filter_kwargs or None, q=q
    )
    if qs is None:
        return None
    qs.set_delete()
    sql, params = qs.compile_sql()
    if not sql:
        return None
    return sql, list(params)


def execute_fetchall(connection, sql: str, params=None) -> list:
    with connection.cursor() as cursor:
        cursor.execute(sql, params or ())
        return cursor.fetchall()


def execute_fetchone_pair(connection, sql: str, params=None):
    with connection.cursor() as cursor:
        cursor.execute(sql, params or ())
        row = cursor.fetchone()
        if row is None:
            return None, False
        extra = cursor.fetchone() is not None
        return row, extra
