"""
Python facade for the native ORM data plane.

Query build + SQL compile live in C++. This module exports schema from Meta
(including reverse FK and M2M), bridges kwargs / Q trees (incl. XOR), and
runs execute/materialize in few crossings.
"""

from __future__ import annotations

import threading
from typing import Any

from django.native._loader import AVAILABLE, get_native_module

__all__ = [
    "AVAILABLE",
    "apply_q",
    "build_queryset",
    "clear_schema",
    "compile_delete",
    "compile_query_plan_update",
    "compile_query_plan_values",
    "compile_select",
    "compile_simple_update",
    "compile_simple_values_filter",
    "compile_update",
    "compile_values_list_get",
    "execute_fetchall",
    "execute_fetchone_pair",
    "export_model",
    "initialize_schema_registry",
    "model_id",
    "q_to_tree",
    "register_model_from_meta",
]


_IMPL = get_native_module()
_ORM = getattr(_IMPL, "orm", None) if _IMPL is not None else None
_MODEL_CACHE_ATTR = "_django_native_orm_model_id"
_schema_generation = 0
_schema_lock = threading.RLock()
_dialect_cache: dict[str, int] = {}
_ADAPTER_TYPES_UNSET = object()
_postgresql_integer_types = _ADAPTER_TYPES_UNSET

# These exact Django classes have preparation semantics that can be reproduced
# by the native primitive binder without calling a Python Field hook. Class
# identity is intentional: a custom IntegerField subclass may override
# get_prep_value(), get_db_prep_value(), or from_db_value().
_DIRECT_FIELD_CLASS_NAMES = frozenset(
    {
        "AutoField",
        "BigAutoField",
        "SmallAutoField",
        "IntegerField",
        "BigIntegerField",
        "SmallIntegerField",
        "PositiveIntegerField",
        "PositiveBigIntegerField",
        "PositiveSmallIntegerField",
        "FloatField",
        "BooleanField",
        "CharField",
        "TextField",
    }
)


def _field_supports_direct_primitive_prep(field) -> bool:
    cls = field.__class__
    return (
        cls.__module__ == "django.db.models.fields"
        and cls.__name__ in _DIRECT_FIELD_CLASS_NAMES
        and not field.is_relation
        and not field.generated
    )


def _orm():
    return _ORM


def clear_schema() -> None:
    global _postgresql_integer_types, _schema_generation

    orm = _orm()
    if orm is not None:
        with _schema_lock:
            orm.clear_schema()
            _schema_generation += 1
            _dialect_cache.clear()
            _postgresql_integer_types = _ADAPTER_TYPES_UNSET


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
    native_direct=False,
    generated=False,
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
        bool(native_direct),
        bool(generated),
    )


def register_model_from_meta(
    model, _seen: set | None = None, *, force: bool = False
) -> int | None:
    """
    Snapshot model._meta into the C++ SchemaRegistry.

    Includes forward FK, reverse FK, and M2M relation hops. Recursively
    exports related models for multi-hop joins.
    """
    cached = getattr(model, _MODEL_CACHE_ATTR, None)
    if not force and cached is not None and cached[0] == _schema_generation:
        return cached[1]

    orm = _orm()
    if orm is None:
        return None
    with _schema_lock:
        cached = getattr(model, _MODEL_CACHE_ATTR, None)
        if not force and cached is not None and cached[0] == _schema_generation:
            return cached[1]
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
                    register_model_from_meta(remote_model, _seen, force=force)
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
                    native_direct=_field_supports_direct_primitive_prep(f),
                    generated=f.generated,
                )
            )

        # Forward M2M (not auto-created reverse side).
        for f in opts.many_to_many:
            if f.auto_created:
                continue
            try:
                remote_model = f.remote_field.model
                register_model_from_meta(remote_model, _seen, force=force)
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
                register_model_from_meta(remote_model, _seen, force=force)
                remote = remote_model._meta
                remote_label = f"{remote.app_label}.{remote.object_name}"
                if rel.many_to_many:
                    # Reverse M2M: through from the field on the other side.
                    field = rel.field
                    through = field.remote_field.through._meta
                    # From this model, through column to us is
                    # m2m_reverse_name on the forward field, and to remote is
                    # m2m_column_name.
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
                    # Reverse FK / O2O.
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

        mid = orm.register_model(label, opts.db_table, fields)
        setattr(model, _MODEL_CACHE_ATTR, (_schema_generation, int(mid)))
        return int(mid)


def export_model(model) -> int | None:
    return register_model_from_meta(model)


def initialize_schema_registry(apps_registry=None) -> None:
    """Export installed models once after the app registry is ready."""
    if _orm() is None:
        return
    if apps_registry is None:
        from django.apps import apps as apps_registry

    for model in apps_registry.get_models(include_auto_created=True):
        register_model_from_meta(model)


def dialect_for_connection(connection) -> int | None:
    orm = _orm()
    if orm is None:
        return None
    vendor = connection.vendor
    dialect = _dialect_cache.get(vendor)
    if dialect is not None:
        return dialect
    if vendor in ("postgresql", "postgres"):
        dialect = int(orm.DIALECT_POSTGRES)
    elif vendor in ("mysql", "mariadb"):
        dialect = int(orm.DIALECT_MYSQL)
    else:
        dialect = int(orm.DIALECT_SQLITE)
    _dialect_cache[vendor] = dialect
    return dialect


def _parameter_types_for_connection(connection):
    """Return cached psycopg integer wrapper types for native parameter output."""
    if connection.vendor not in ("postgresql", "postgres"):
        return None
    global _postgresql_integer_types
    if _postgresql_integer_types is _ADAPTER_TYPES_UNSET:
        ops = getattr(connection, "ops", None)
        type_map = getattr(ops, "integerfield_type_map", None)
        if type_map is None:
            # psycopg2 and nonstandard PostgreSQL backends don't use the
            # psycopg 3 integer wrapper classes.
            _postgresql_integer_types = None
        else:
            try:
                _postgresql_integer_types = (
                    type_map["SmallIntegerField"],
                    type_map["IntegerField"],
                    type_map["BigIntegerField"],
                )
            except (KeyError, TypeError):
                _postgresql_integer_types = None
    return _postgresql_integer_types


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
    if q is not None:
        tree = q_to_tree(q)
        if tree is None:
            return None
        qs = orm.QuerySet.create_from_q(int(mid), int(dialect), tree)
        if qs is None:
            return None
    else:
        qs = orm.QuerySet.create(int(mid), int(dialect))
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
    orm = _orm()
    if orm is None:
        return None
    mid = register_model_from_meta(model)
    dialect = dialect_for_connection(connection)
    if mid is None or dialect is None or not field_names:
        return None
    compiled = orm.compile_simple_values_get(
        int(mid),
        int(dialect),
        field_names,
        lookup_field,
        lookup_value,
        int(limit),
        _parameter_types_for_connection(connection),
    )
    if compiled is None:
        return None
    return compiled


def compile_simple_values_filter(
    model,
    connection,
    *,
    field_names,
    lookup_field: str,
    lookup_values,
    lookup_in: bool,
    ordering_names=(),
    limit: int = 0,
    offset: int = 0,
) -> tuple[str, list] | None:
    """Compile a single-table exact/IN projection in one native call."""
    orm = _orm()
    if orm is None or not field_names or not lookup_values:
        return None
    mid = register_model_from_meta(model)
    dialect = dialect_for_connection(connection)
    if mid is None or dialect is None:
        return None
    return orm.compile_simple_values_filter(
        int(mid),
        int(dialect),
        field_names,
        lookup_field,
        bool(lookup_in),
        lookup_values,
        ordering_names,
        int(limit),
        int(offset),
        _parameter_types_for_connection(connection),
    )


def compile_simple_update(
    model,
    connection,
    *,
    lookup_field: str,
    lookup_value,
    update_names,
    update_values,
) -> tuple[str, list] | None:
    """Compile one exact filter and all assignments in one native call."""
    orm = _orm()
    if orm is None or not update_names:
        return None
    mid = register_model_from_meta(model)
    dialect = dialect_for_connection(connection)
    if mid is None or dialect is None:
        return None
    return orm.compile_simple_update(
        int(mid),
        int(dialect),
        lookup_field,
        lookup_value,
        update_names,
        update_values,
        _parameter_types_for_connection(connection),
    )


def compile_query_plan_values(
    model,
    connection,
    plan,
    *,
    limit: int = 0,
    offset: int = 0,
) -> tuple[str, list] | None:
    """Compile a compact C++ exact/IN projection plan in one crossing."""
    orm = _orm()
    if orm is None or plan is None:
        return None
    mid = register_model_from_meta(model)
    dialect = dialect_for_connection(connection)
    if mid is None or dialect is None:
        return None
    return orm.compile_simple_values_plan(
        int(mid),
        int(dialect),
        plan,
        int(limit),
        int(offset),
        _parameter_types_for_connection(connection),
    )


def compile_query_plan_update(
    model,
    connection,
    plan,
    *,
    updates,
) -> tuple[str, list] | None:
    """Compile a compact C++ exact-filter update plan in one crossing."""
    orm = _orm()
    if orm is None or plan is None or not updates:
        return None
    mid = register_model_from_meta(model)
    dialect = dialect_for_connection(connection)
    if mid is None or dialect is None:
        return None
    return orm.compile_simple_update_plan(
        int(mid),
        int(dialect),
        plan,
        updates,
        _parameter_types_for_connection(connection),
    )


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
    if field_names and not kwargs and q is None and (limit is None or limit > 0):
        orm = _orm()
        if orm is None:
            return None
        mid = register_model_from_meta(model)
        dialect = dialect_for_connection(connection)
        if mid is None or dialect is None:
            return None
        compiled = orm.compile_simple_values_select(
            int(mid), int(dialect), field_names, (), int(limit or 0), 0
        )
        return compiled
    qs = build_queryset(model, connection, kwargs=kwargs or None, q=q)
    if qs is None:
        return None
    if field_names:
        if not qs.values_list(list(field_names), flat):
            return None
    if limit is not None:
        qs.set_limit(int(limit))
    sql, params = qs.compile_sql(_parameter_types_for_connection(connection))
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
    compiled = qs.compile_update_kwargs(
        update_kwargs, _parameter_types_for_connection(connection)
    )
    if compiled is None:
        return None
    sql, params = compiled
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
    sql, params = qs.compile_sql(_parameter_types_for_connection(connection))
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
    Projects annotation aliases onto instances (setattr).
    Runs native secondary queries for prefetch_specs when possible.
    """
    try:
        qs = handle.clone()
        if not qs.base_attnames():
            if not qs.select_model_columns():
                return None
        if limit is not None:
            qs.set_limit(int(limit))
        sql, params = qs.compile_sql(_parameter_types_for_connection(connection))
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
        try:
            ann_info = list(qs.annotation_selects())
        except Exception:
            ann_info = []
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
            # Project annotation columns onto the instance (Django ModelIterable).
            for ann in ann_info:
                off = int(ann["offset"])
                if off < len(row):
                    setattr(obj, ann["alias"], row[off])
            objs.append(obj)
        # Native prefetch secondary queries (multi-hop sequential).
        if objs and prefetch_specs:
            _run_native_prefetch(model, objs, handle, prefetch_specs, connection)
            # Mark lookups done so Django prefetch skips them
            prefetches = []
        return objs, prefetches
    except Exception:
        return None


def _collect_prefetched_at_path(roots, path):
    """Walk root instances along a multi-hop prefetch path; return leaf objects."""
    if not path:
        return list(roots)
    current = list(roots)
    for part in path.split("__"):
        nxt = []
        for obj in current:
            cache = getattr(obj, "_prefetched_objects_cache", None) or {}
            val = cache.get(part)
            if val is None:
                # Single-object forward FK may be on field cache
                try:
                    field = obj._meta.get_field(part)
                    if hasattr(field, "is_cached") and field.is_cached(obj):
                        rel = field.get_cached_value(obj)
                        if rel is not None:
                            nxt.append(rel)
                except Exception:
                    pass
                continue
            if isinstance(val, (list, tuple)):
                nxt.extend(val)
            else:
                nxt.append(val)
        current = nxt
    return current


def _attach_prefetched(parent, cache_name, rel_list_or_obj, many=True):
    """Store prefetched related objects on parent (Django cache convention)."""
    if not many:
        try:
            field = parent._meta.get_field(cache_name)
            if hasattr(field, "set_cached_value"):
                field.set_cached_value(parent, rel_list_or_obj)
                return
        except Exception:
            pass
    if not hasattr(parent, "_prefetched_objects_cache"):
        parent._prefetched_objects_cache = {}
    parent._prefetched_objects_cache[cache_name] = rel_list_or_obj


def _run_native_prefetch(model, objs, handle, specs, connection):
    """
    Execute secondary SELECTs for reverse FK / M2M / forward FK and attach.

    Specs are ordered hop-by-hop for multi-hop lookups. parent_path empty
    means attach to root objs; otherwise walk prior prefetches.
    """
    if not objs:
        return
    db = connection.alias
    roots = objs

    for spec in specs:
        try:
            parent_path = spec.get("parent_path") or ""
            parents = _collect_prefetched_at_path(roots, parent_path)
            if not parents:
                continue
            # Parent model for this hop: roots' model or last hop remote.
            if parent_path:
                parent_model = parents[0].__class__
            else:
                parent_model = model
            pk_att = parent_model._meta.pk.attname
            parent_pks = [getattr(o, pk_att) for o in parents]
            pk_to_obj = {getattr(o, pk_att): o for o in parents}

            compiled = handle.compile_prefetch_secondary(spec, parent_pks)
            # Back-compat: 2-tuple or 3-tuple (sql, params, parent_link_offset)
            if compiled is None:
                continue
            if len(compiled) == 3:
                sql, params, parent_link_offset = compiled
            else:
                sql, params = compiled
                parent_link_offset = -1
            if not sql:
                from django.db.models.query import prefetch_related_objects

                prefetch_related_objects(roots, spec["lookup"])
                continue
            rows = execute_fetchall(connection, sql, params)
            rel_model = None
            label = spec.get("remote_model_label") or ""
            if label and "." in label:
                app, name = label.split(".", 1)
                from django.apps import apps

                rel_model = apps.get_model(app, name)
            if rel_model is None:
                continue
            remote_atts = [
                f.attname for f in rel_model._meta.concrete_fields if f.column
            ]
            rel = spec.get("rel") or ""
            cache = spec.get("cache_name") or spec.get("hop") or spec.get("lookup")
            buckets = {pk: [] for pk in parent_pks}

            if rel in ("fk", "forward_fk"):
                # Forward FK: parents hold FK values; attach single related obj.
                by_pk = {}
                for row in rows:
                    rel_obj = rel_model.from_db(
                        db, remote_atts, row[: len(remote_atts)]
                    )
                    by_pk[rel_obj.pk] = rel_obj
                try:
                    field = parent_model._meta.get_field(cache)
                except Exception:
                    field = None
                for po in parents:
                    if field is None:
                        continue
                    fk_val = getattr(po, field.attname, None)
                    rel_obj = by_pk.get(fk_val)
                    if rel_obj is not None:
                        _attach_prefetched(po, cache, rel_obj, many=False)
                continue

            for row in rows:
                rel_obj = rel_model.from_db(db, remote_atts, row[: len(remote_atts)])
                parent_id = None
                if rel in ("rev_fk", "reverse_fk"):
                    fk_col = spec.get("remote_fk_column") or ""
                    for f in rel_model._meta.concrete_fields:
                        if f.column == fk_col:
                            parent_id = getattr(rel_obj, f.attname)
                            break
                elif rel in ("m2m", "rev_m2m", "forward_m2m", "reverse_m2m"):
                    off = int(parent_link_offset)
                    if off < 0:
                        # Convention: parent link is the column after remote atts
                        off = len(remote_atts)
                    if off < len(row):
                        parent_id = row[off]
                    else:
                        from django.db.models.query import prefetch_related_objects

                        prefetch_related_objects(roots, spec["lookup"])
                        buckets = None
                        break
                if parent_id is not None and parent_id in buckets:
                    buckets[parent_id].append(rel_obj)
            if buckets is None:
                continue
            for pk, rel_list in buckets.items():
                parent = pk_to_obj.get(pk)
                if parent is None:
                    continue
                _attach_prefetched(parent, cache, rel_list, many=True)
        except Exception:
            try:
                from django.db.models.query import prefetch_related_objects

                prefetch_related_objects(objs, spec.get("lookup", ""))
            except Exception:
                pass


def materialize_aggregate(connection, handle):
    """Run aggregate-style select; return dict alias→value or None."""
    try:
        sql, params = handle.compile_sql(_parameter_types_for_connection(connection))
        if not sql:
            return None
        rows = execute_fetchall(connection, sql, params)
        if not rows:
            return {}
        # Alias order follows select list; Python caller passes alias names.
        return rows[0]
    except Exception:
        return None
