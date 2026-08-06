"""
Python facade for the native ORM data plane.

Query build + SQL compile live in C++. This module exports schema from Meta
(including reverse FK and M2M), bridges kwargs / Q trees (incl. XOR), and
runs execute/materialize in few crossings.
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


def _field_row(
    name,
    attname,
    column,
    class_name,
    pk,
    null,
    remote_table="",
    remote_pk="",
    remote_label="",
    rel_kind="",
    m2m_table="",
    m2m_column="",
    m2m_reverse="",
    remote_fk_column="",
):
    return (
        name,
        attname,
        column,
        class_name,
        bool(pk),
        bool(null),
        remote_table or "",
        remote_pk or "",
        remote_label or "",
        rel_kind or "",
        m2m_table or "",
        m2m_column or "",
        m2m_reverse or "",
        remote_fk_column or "",
    )


def register_model_from_meta(model, _seen: set | None = None) -> int | None:
    """
    Snapshot model._meta into the C++ SchemaRegistry.

    Includes forward FK, reverse FK, and M2M relation hops. Recursively
    exports related models for multi-hop joins.
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

    # Concrete local fields (columns + forward FK).
    for f in opts.concrete_fields:
        if not f.column:
            continue
        remote_table = remote_pk = remote_label = rel_kind = ""
        if f.is_relation and (f.many_to_one or f.one_to_one):
            try:
                remote_model = f.remote_field.model
                register_model_from_meta(remote_model, _seen)
                remote = remote_model._meta
                remote_table = remote.db_table
                remote_pk = remote.pk.column
                remote_label = f"{remote.app_label}.{remote.object_name}"
                rel_kind = "fk"
            except Exception:
                pass
        fields.append(
            _field_row(
                f.name,
                f.attname,
                f.column,
                f.__class__.__name__,
                f.primary_key,
                f.null,
                remote_table,
                remote_pk,
                remote_label,
                rel_kind,
            )
        )

    # Forward M2M (not auto-created reverse side).
    for f in opts.many_to_many:
        if f.auto_created:
            continue
        try:
            remote_model = f.remote_field.model
            register_model_from_meta(remote_model, _seen)
            remote = remote_model._meta
            through = f.remote_field.through._meta
            # Columns on through pointing to each side.
            m2m_column = f.m2m_column_name()
            m2m_reverse = f.m2m_reverse_name()
            fields.append(
                _field_row(
                    f.name,
                    f.name,
                    "",
                    "ManyToManyField",
                    False,
                    True,
                    remote.db_table,
                    remote.pk.column,
                    f"{remote.app_label}.{remote.object_name}",
                    "m2m",
                    through.db_table,
                    m2m_column,
                    m2m_reverse,
                    "",
                )
            )
        except Exception:
            continue

    # Reverse relations (reverse FK and reverse M2M).
    for rel in opts.related_objects:
        accessor = rel.get_accessor_name()
        if not accessor:
            continue
        try:
            remote_model = rel.related_model
            register_model_from_meta(remote_model, _seen)
            remote = remote_model._meta
            remote_label = f"{remote.app_label}.{remote.object_name}"
            if rel.many_to_many:
                # Reverse M2M: through from the field on the other side.
                field = rel.field
                through = field.remote_field.through._meta
                # From this model, through column to us is m2m_reverse_name on
                # the forward field, and to remote is m2m_column_name.
                m2m_column = field.m2m_reverse_name()
                m2m_reverse = field.m2m_column_name()
                fields.append(
                    _field_row(
                        accessor,
                        accessor,
                        "",
                        "ManyToManyRel",
                        False,
                        True,
                        remote.db_table,
                        remote.pk.column,
                        remote_label,
                        "rev_m2m",
                        through.db_table,
                        m2m_column,
                        m2m_reverse,
                        "",
                    )
                )
            else:
                # Reverse FK / O2O
                fields.append(
                    _field_row(
                        accessor,
                        accessor,
                        "",
                        "ManyToOneRel",
                        False,
                        True,
                        remote.db_table,
                        remote.pk.column,
                        remote_label,
                        "rev_fk",
                        "",
                        "",
                        "",
                        rel.field.column,
                    )
                )
        except Exception:
            continue

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
    Lower a Django Q (or (key, value) child) to a tree for C++ apply_q.

    Supports AND / OR / XOR / NOT and simple value atoms. Returns None on
    unsupported nodes (F(), subqueries, …).
    """
    from django.db.models import Q
    from django.db.models.lookups import Lookup

    if isinstance(node, Q):
        connector = node.connector or Q.AND
        if connector == Q.XOR:
            kind = "xor"
        elif connector == Q.OR:
            kind = "or"
        else:
            kind = "and"
        children = []
        for c in node.children:
            lc = q_to_tree(c)
            if lc is None:
                return None
            children.append(lc)
        body = {"kind": kind, "children": children}
        if node.negated:
            return {"kind": "not", "children": [body]}
        return body

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
            return None
        return {"kind": "atom", "key": key, "values": values}

    if isinstance(node, Lookup):
        return None
    return None


def apply_q(qs, q_obj) -> bool:
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
    qs = build_queryset(model, connection, kwargs=filter_kwargs or None, q=q)
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
    qs = build_queryset(model, connection, kwargs=filter_kwargs or None, q=q)
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


def materialize_models(model, connection, handle, *, limit=None):
    """
    Compile+fetch model rows from a native handle.
    Returns (list[model], prefetch_lookups) or None on failure.
    Applies select_related caches when related_selects_info is present.
    Runs native secondary queries for prefetch_specs when possible.
    """
    try:
        qs = handle.clone()
        if not qs.base_attnames():
            if not qs.select_model_columns():
                return None
        if limit is not None:
            qs.set_limit(int(limit))
        sql, params = qs.compile_sql()
        if not sql:
            return None
        rows = execute_fetchall(connection, sql, params)
        attnames = list(qs.base_attnames())
        related = list(qs.related_selects_info())
        prefetches = list(qs.prefetch_lookups())
        try:
            prefetch_specs = list(qs.prefetch_specs())
        except Exception:
            prefetch_specs = []
        db = connection.alias
        objs = []
        for row in rows:
            base = row[: len(attnames)]
            obj = model.from_db(db, attnames, base)
            for rs in related:
                off = int(rs["offset"])
                cnt = int(rs["count"])
                rel_atts = list(rs["attnames"])
                rel_vals = row[off : off + cnt]
                if not rel_vals or rel_vals[0] is None:
                    continue
                path = rs["path"]
                rel_model = model
                field = None
                for part in path.split("__"):
                    field = rel_model._meta.get_field(part)
                    rel_model = field.related_model
                rel_obj = rel_model.from_db(db, rel_atts, rel_vals)
                if field is not None:
                    if hasattr(field, "set_cached_value"):
                        field.set_cached_value(obj, rel_obj)
                    elif hasattr(field, "get_cache_name"):
                        setattr(obj, field.get_cache_name(), rel_obj)
            objs.append(obj)
        # Native prefetch secondary queries
        if objs and prefetch_specs:
            _run_native_prefetch(model, objs, handle, prefetch_specs, connection)
            # Mark lookups done so Django prefetch skips them
            prefetches = []
        return objs, prefetches
    except Exception:
        return None


def _run_native_prefetch(model, objs, handle, specs, connection):
    """Execute secondary SELECTs for reverse FK / M2M and attach to parents."""
    if not objs:
        return
    # Parent PKs
    pk_att = model._meta.pk.attname
    parent_pks = [getattr(o, pk_att) for o in objs]
    pk_to_obj = {getattr(o, pk_att): o for o in objs}
    db = connection.alias

    for spec in specs:
        try:
            sql, params = handle.compile_prefetch_secondary(spec, parent_pks)
            if not sql:
                # Fall back to Django prefetch for this lookup
                from django.db.models.query import prefetch_related_objects

                prefetch_related_objects(objs, spec["lookup"])
                continue
            rows = execute_fetchall(connection, sql, params)
            # Resolve remote model
            rel_model = None
            label = spec.get("remote_model_label") or ""
            if label and "." in label:
                app, name = label.split(".", 1)
                from django.apps import apps

                rel_model = apps.get_model(app, name)
            if rel_model is None:
                continue
            # Concrete attnames order from secondary select_model_columns
            remote_atts = [
                f.attname
                for f in rel_model._meta.concrete_fields
                if f.column
            ]
            # Group related rows by parent link
            rel = spec.get("rel") or ""
            cache = spec.get("cache_name") or spec.get("lookup")
            # For reverse FK, FK col is remote_fk_column on related row
            # For M2M, need parent id from through — secondary SQL selects only
            # remote columns; for M2M we must include through parent col.
            # Current compile only selects remote model columns — for reverse FK
            # the FK value is among remote_atts (author_id).
            buckets = {pk: [] for pk in parent_pks}
            for row in rows:
                rel_obj = rel_model.from_db(db, remote_atts, row[: len(remote_atts)])
                parent_id = None
                if rel in ("rev_fk", "reverse_fk"):
                    fk_col = spec.get("remote_fk_column") or ""
                    # map column → attname
                    for f in rel_model._meta.concrete_fields:
                        if f.column == fk_col:
                            parent_id = getattr(rel_obj, f.attname)
                            break
                elif rel in ("m2m", "rev_m2m", "forward_m2m", "reverse_m2m"):
                    # Without through parent id in SELECT, cannot bucket natively;
                    # fall back.
                    from django.db.models.query import prefetch_related_objects

                    prefetch_related_objects(objs, spec["lookup"])
                    buckets = None
                    break
                elif rel in ("fk", "forward_fk"):
                    parent_id = rel_obj.pk  # parent stored FK pointing here
                    # Actually for forward FK prefetch on parent, children are
                    # where remote.pk in parent.fk_values — attach by pk match
                    # to parents that have that fk.
                    for po in objs:
                        # find field
                        try:
                            field = model._meta.get_field(cache)
                            if getattr(po, field.attname) == rel_obj.pk:
                                if hasattr(field, "set_cached_value"):
                                    field.set_cached_value(po, rel_obj)
                        except Exception:
                            pass
                    continue
                if parent_id in buckets:
                    buckets[parent_id].append(rel_obj)
            if buckets is None:
                continue
            # Attach reverse relations as lists on cache / descriptor
            for pk, rel_list in buckets.items():
                parent = pk_to_obj.get(pk)
                if parent is None:
                    continue
                try:
                    field = model._meta.get_field(cache)
                except Exception:
                    # related descriptor name
                    setattr(parent, "_prefetched_objects_cache", getattr(parent, "_prefetched_objects_cache", {}))
                    parent._prefetched_objects_cache[cache] = rel_list
                    continue
                # Reverse many: use prefetched cache
                cache_name = cache
                if not hasattr(parent, "_prefetched_objects_cache"):
                    parent._prefetched_objects_cache = {}
                parent._prefetched_objects_cache[cache_name] = rel_list
        except Exception:
            try:
                from django.db.models.query import prefetch_related_objects

                prefetch_related_objects(objs, spec.get("lookup", ""))
            except Exception:
                pass


def materialize_aggregate(connection, handle):
    """Run aggregate-style select; return dict alias→value or None."""
    try:
        sql, params = handle.compile_sql()
        if not sql:
            return None
        rows = execute_fetchall(connection, sql, params)
        if not rows:
            return {}
        # Alias order follows select list; Python caller passes alias names.
        return rows[0]
    except Exception:
        return None
