"""
The main QuerySet implementation. This provides the public API for the ORM.
"""

import copy
import operator
import warnings
from contextlib import nullcontext
from functools import reduce
from itertools import chain, islice

from asgiref.sync import sync_to_async

import django
from django.conf import settings
from django.core import exceptions
from django.db import (
    DJANGO_VERSION_PICKLE_KEY,
    IntegrityError,
    NotSupportedError,
    connections,
    router,
    transaction,
)
from django.db.models import AutoField, DateField, DateTimeField, Field, Max, sql
from django.db.models.constants import LOOKUP_SEP, OnConflict
from django.db.models.deletion import Collector
from django.db.models.expressions import Case, DatabaseDefault, F, Value, When
from django.db.models.functions import Cast, Trunc
from django.db.models.query_utils import FilteredRelation, Q
from django.db.models.sql.constants import GET_ITERATOR_CHUNK_SIZE, ROW_COUNT
from django.db.models.utils import (
    AltersData,
    create_namedtuple_class,
    resolve_callables,
)
from django.native._loader import AVAILABLE as _NATIVE_AVAILABLE
from django.utils import timezone
from django.utils.deprecation import RemovedInDjango70Warning
from django.utils.functional import cached_property

# The maximum number of results to fetch in a get() query.
MAX_GET_RESULTS = 21

# The maximum number of items to display in a QuerySet.__repr__
REPR_OUTPUT_SIZE = 20

PROHIBITED_FILTER_KWARGS = frozenset(["_connector", "_negated"])

# SQL templates used by the model-save fast path in django.db.models.base.
# QuerySet terminal shapes use the generation-aware native cache instead.
_SIMPLE_SQL_CACHE = {}
_FAST_PATH_MISS = object()


def _q_from_native_tree(node):
    """Rebuild an equivalent Django Q object from a C++ QueryPlan node."""
    kind = node["kind"]
    if kind == "atom":
        values = node.get("values", ())
        key = node["key"]
        if key.rsplit(LOOKUP_SEP, 1)[-1] == "in":
            value = list(values)
        elif len(values) == 1:
            value = values[0]
        else:
            value = list(values)
        return Q((key, value))

    children = [_q_from_native_tree(child) for child in node.get("children", ())]
    if kind == "not":
        if len(children) != 1:
            raise ValueError("native NOT plan must have exactly one child")
        return ~children[0]
    connector = Q.AND
    if kind == "or":
        connector = Q.OR
    elif kind == "xor":
        connector = Q.XOR
    return Q(*children, _connector=connector)


class BaseIterable:
    def __init__(
        self, queryset, chunked_fetch=False, chunk_size=GET_ITERATOR_CHUNK_SIZE
    ):
        self.queryset = queryset
        self.chunked_fetch = chunked_fetch
        self.chunk_size = chunk_size

    async def _async_generator(self):
        # Generators don't actually start running until the first time you call
        # next() on them, so make the generator object in the async thread and
        # then repeatedly dispatch to it in a sync thread.
        sync_generator = self.__iter__()

        def next_slice(gen):
            return list(islice(gen, self.chunk_size))

        while True:
            chunk = await sync_to_async(next_slice)(sync_generator)
            for item in chunk:
                yield item
            if len(chunk) < self.chunk_size:
                break

    # __aiter__() is a *synchronous* method that has to then return an
    # *asynchronous* iterator/generator. Thus, nest an async generator inside
    # it.
    # This is a generic iterable converter for now, and is going to suffer a
    # performance penalty on large sets of items due to the cost of crossing
    # over the sync barrier for each chunk. Custom __aiter__() methods should
    # be added to each Iterable subclass, but that needs some work in the
    # Compiler first.
    def __aiter__(self):
        return self._async_generator()


class ModelIterable(BaseIterable):
    """Iterable that yields a model instance for each row."""

    def __iter__(self):
        queryset = self.queryset
        db = queryset.db
        compiler = queryset.query.get_compiler(using=db)
        # Execute the query. This will also fill compiler.select, klass_info,
        # and annotations.
        results = compiler.execute_sql(
            chunked_fetch=self.chunked_fetch, chunk_size=self.chunk_size
        )
        select, klass_info, annotation_col_map = (
            compiler.select,
            compiler.klass_info,
            compiler.annotation_col_map,
        )
        model_cls = klass_info["model"]
        select_fields = klass_info["select_fields"]
        model_fields_start, model_fields_end = select_fields[0], select_fields[-1] + 1
        init_list = [
            f[0].target.attname for f in select[model_fields_start:model_fields_end]
        ]
        related_populators = get_related_populators(klass_info, select, db)
        known_related_objects = [
            (
                field,
                related_objs,
                operator.attrgetter(
                    *[
                        (
                            field.attname
                            if from_field == "self"
                            else queryset.model._meta.get_field(from_field).attname
                        )
                        for from_field in field.from_fields
                    ]
                ),
            )
            for field, related_objs in queryset._known_related_objects.items()
        ]
        for row in compiler.results_iter(results):
            obj = model_cls.from_db(
                db, init_list, row[model_fields_start:model_fields_end]
            )
            for rel_populator in related_populators:
                rel_populator.populate(row, obj)
            if annotation_col_map:
                for attr_name, col_pos in annotation_col_map.items():
                    setattr(obj, attr_name, row[col_pos])

            # Add the known related objects to the model.
            for field, rel_objs, rel_getter in known_related_objects:
                # Avoid overwriting objects loaded by, e.g., select_related().
                if field.is_cached(obj):
                    continue
                rel_obj_id = rel_getter(obj)
                try:
                    rel_obj = rel_objs[rel_obj_id]
                except KeyError:
                    pass  # May happen in qs1 | qs2 scenarios.
                else:
                    setattr(obj, field.name, rel_obj)

            yield obj


class RawModelIterable(BaseIterable):
    """
    Iterable that yields a model instance for each row from a raw queryset.
    """

    def __iter__(self):
        # Cache some things for performance reasons outside the loop.
        db = self.queryset.db
        query = self.queryset.query
        connection = connections[db]
        compiler = connection.ops.compiler("SQLCompiler")(query, connection, db)
        query_iterator = iter(query)

        try:
            (
                model_init_names,
                model_init_pos,
                annotation_fields,
            ) = self.queryset.resolve_model_init_order()
            model_cls = self.queryset.model
            if any(
                f.attname not in model_init_names for f in model_cls._meta.pk_fields
            ):
                raise exceptions.FieldDoesNotExist(
                    "Raw query must include the primary key"
                )
            fields = [self.queryset.model_fields.get(c) for c in self.queryset.columns]
            cols = [f.get_col(f.model._meta.db_table) if f else None for f in fields]
            converters = compiler.get_converters(cols)
            if converters:
                query_iterator = compiler.apply_converters(query_iterator, converters)
            if compiler.has_composite_fields(cols):
                query_iterator = compiler.composite_fields_to_tuples(
                    query_iterator, cols
                )
            for values in query_iterator:
                # Associate fields to values
                model_init_values = [values[pos] for pos in model_init_pos]
                instance = model_cls.from_db(db, model_init_names, model_init_values)
                if annotation_fields:
                    for column, pos in annotation_fields:
                        setattr(instance, column, values[pos])
                yield instance
        finally:
            # Done iterating the Query. If it has its own cursor, close it.
            if hasattr(query, "cursor") and query.cursor:
                query.cursor.close()


class ValuesIterable(BaseIterable):
    """
    Iterable returned by QuerySet.values() that yields a dict for each row.
    """

    def __iter__(self):
        queryset = self.queryset
        query = queryset.query
        compiler = query.get_compiler(queryset.db)

        if query.selected:
            names = list(query.selected)
        else:
            # extra(select=...) cols are always at the start of the row.
            names = [
                *query.extra_select,
                *query.values_select,
                *query.annotation_select,
            ]
        indexes = range(len(names))
        for row in compiler.results_iter(
            chunked_fetch=self.chunked_fetch, chunk_size=self.chunk_size
        ):
            yield {names[i]: row[i] for i in indexes}


class ValuesListIterable(BaseIterable):
    """
    Iterable returned by QuerySet.values_list(flat=False) that yields a tuple
    for each row.
    """

    def __iter__(self):
        queryset = self.queryset
        query = queryset.query
        compiler = query.get_compiler(queryset.db)
        return compiler.results_iter(
            tuple_expected=True,
            chunked_fetch=self.chunked_fetch,
            chunk_size=self.chunk_size,
        )


class NamedValuesListIterable(ValuesListIterable):
    """
    Iterable returned by QuerySet.values_list(named=True) that yields a
    namedtuple for each row.
    """

    def __iter__(self):
        queryset = self.queryset
        if queryset._fields:
            names = queryset._fields
        else:
            query = queryset.query
            names = [
                *query.extra_select,
                *query.values_select,
                *query.annotation_select,
            ]
        tuple_class = create_namedtuple_class(*names)
        new = tuple.__new__
        for row in super().__iter__():
            yield new(tuple_class, row)


class FlatValuesListIterable(BaseIterable):
    """
    Iterable returned by QuerySet.values_list(flat=True) that yields single
    values.
    """

    def __iter__(self):
        queryset = self.queryset
        compiler = queryset.query.get_compiler(queryset.db)
        for row in compiler.results_iter(
            chunked_fetch=self.chunked_fetch, chunk_size=self.chunk_size
        ):
            yield row[0]


class QuerySet(AltersData):
    """Represent a lazy database lookup for a set of objects."""

    def __init__(self, model=None, query=None, using=None, hints=None):
        self.model = model
        self._db = using
        self._hints = hints or {}
        # Native-supported chains don't need Django's mutable Query graph.
        # Leave it unallocated until introspection or a fallback operation
        # actually needs it. The pure-Python configuration remains eager.
        if query is not None:
            self._query = query
        elif _NATIVE_AVAILABLE:
            self._query = None
        else:
            self._query = sql.Query(self.model)
        self._result_cache = None
        self._sticky_filter = False
        self._for_write = False
        self._prefetch_related_lookups = ()
        self._prefetch_done = False
        self._known_related_objects = {}  # {rel_field: {pk: rel_obj}}
        self._iterable_class = ModelIterable
        self._fields = None
        self._defer_next_filter = False
        self._deferred_filter = None
        # C++ ORM data-plane handle (optional). Poisoned via _native_disabled.
        self._native_qs = None
        self._native_disabled = False
        # Supported native chains keep C++ as the authoritative query graph.
        # QueryPlan owns replay strings and values without retaining Python
        # dicts, tuples, Q objects, or Field objects.
        self._native_authoritative = False
        self._native_plan = None

    @property
    def query(self):
        if self._native_authoritative:
            self._native_materialize_python_query()
        if self._query is None:
            self._query = sql.Query(self.model)
        if self._deferred_filter:
            negate, args, kwargs = self._deferred_filter
            self._filter_or_exclude_inplace(negate, args, kwargs)
            self._native_disabled = True
            self._native_qs = None
            self._deferred_filter = None
        return self._query

    @query.setter
    def query(self, value):
        if value is not None and value.values_select:
            self._iterable_class = ValuesIterable
        self._query = value
        self._native_authoritative = False
        self._native_plan = None

    def as_manager(cls):
        # Address the circular dependency between `Queryset` and `Manager`.
        from django.db.models.manager import Manager

        manager = Manager.from_queryset(cls)()
        manager._built_with_as_manager = True
        return manager

    as_manager.queryset_only = True
    as_manager = classmethod(as_manager)

    ########################
    # PYTHON MAGIC METHODS #
    ########################

    def __deepcopy__(self, memo):
        """Don't populate the QuerySet's cache."""
        obj = self.__class__()
        for k, v in self.__dict__.items():
            if k == "_result_cache":
                obj.__dict__[k] = None
            elif k in {"_native_plan", "_native_qs"}:
                # Both native wrappers use functional copy-on-write updates.
                obj.__dict__[k] = v
            else:
                obj.__dict__[k] = copy.deepcopy(v, memo)
        return obj

    def __getstate__(self):
        # Force the cache to be fully populated.
        self._fetch_all()
        # Nanobind wrappers aren't part of Django's pickle contract. Replay
        # the compact plan once and pickle the equivalent Python Query.
        if self._native_authoritative:
            self.query
        return {**self.__dict__, DJANGO_VERSION_PICKLE_KEY: django.__version__}

    def __setstate__(self, state):
        pickled_version = state.get(DJANGO_VERSION_PICKLE_KEY)
        if pickled_version:
            if pickled_version != django.__version__:
                warnings.warn(
                    "Pickled queryset instance's Django version %s does not "
                    "match the current version %s."
                    % (pickled_version, django.__version__),
                    RuntimeWarning,
                    stacklevel=2,
                )
        else:
            warnings.warn(
                "Pickled queryset instance's Django version is not specified.",
                RuntimeWarning,
                stacklevel=2,
            )
        self.__dict__.update(state)

    def __repr__(self):
        data = list(self[: REPR_OUTPUT_SIZE + 1])
        if len(data) > REPR_OUTPUT_SIZE:
            data[-1] = "...(remaining elements truncated)..."
        return "<%s %r>" % (self.__class__.__name__, data)

    def __len__(self):
        self._fetch_all()
        return len(self._result_cache)

    def __iter__(self):
        """
        The queryset iterator protocol uses three nested iterators in the
        default case:
            1. sql.compiler.execute_sql()
               - Returns 100 rows at time (constants.GET_ITERATOR_CHUNK_SIZE)
                 using cursor.fetchmany(). This part is responsible for
                 doing some column masking, and returning the rows in chunks.
            2. sql.compiler.results_iter()
               - Returns one row at time. At this point the rows are still just
                 tuples. In some cases the return values are converted to
                 Python values at this location.
            3. self.iterator()
               - Responsible for turning the rows into model objects.
        """
        self._fetch_all()
        return iter(self._result_cache)

    def __aiter__(self):
        # Remember, __aiter__ itself is synchronous, it's the thing it returns
        # that is async!
        async def generator():
            await sync_to_async(self._fetch_all)()
            for item in self._result_cache:
                yield item

        return generator()

    def __bool__(self):
        from django import native as _native

        self._fetch_all()
        if _native.AVAILABLE:
            return _native.queryset_cache_truthy(len(self._result_cache))
        return bool(self._result_cache)

    def __getitem__(self, k):
        """Retrieve an item or slice from the set of results."""
        from django import native as _native

        is_int = isinstance(k, int)
        is_slice = isinstance(k, slice)
        if _native.AVAILABLE:
            has_negative = (is_int and k < 0) or (
                is_slice
                and (
                    (k.start is not None and k.start < 0)
                    or (k.stop is not None and k.stop < 0)
                )
            )
            code = _native.queryset_index_validate(is_int, is_slice, has_negative)
            if code == 1:
                raise TypeError(
                    "QuerySet indices must be integers or slices, not %s."
                    % type(k).__name__
                )
            if code == 2:
                raise ValueError("Negative indexing is not supported.")
        else:
            if not is_int and not is_slice:
                raise TypeError(
                    "QuerySet indices must be integers or slices, not %s."
                    % type(k).__name__
                )
            if (is_int and k < 0) or (
                is_slice
                and (
                    (k.start is not None and k.start < 0)
                    or (k.stop is not None and k.stop < 0)
                )
            ):
                raise ValueError("Negative indexing is not supported.")

        if self._result_cache is not None:
            return self._result_cache[k]

        if isinstance(k, slice):
            qs = self._chain()
            if k.start is not None:
                start = int(k.start)
            else:
                start = None
            if k.stop is not None:
                stop = int(k.stop)
            else:
                stop = None
            qs.query.set_limits(start, stop)
            return list(qs)[:: k.step] if k.step else qs

        qs = self._chain()
        qs.query.set_limits(k, k + 1)
        qs._fetch_all()
        return qs._result_cache[0]

    def __class_getitem__(cls, *args, **kwargs):
        return cls

    def __and__(self, other):
        from django import native as _native

        self._check_operator_queryset(other, "&")
        self._merge_sanity_check(other)
        if _native.AVAILABLE:
            kind = _native.qs_and_empty_kind(
                isinstance(self, EmptyQuerySet), isinstance(other, EmptyQuerySet)
            )
            if kind == 1:
                return other
            if kind == 2:
                return self
        else:
            if isinstance(other, EmptyQuerySet):
                return other
            if isinstance(self, EmptyQuerySet):
                return self
        combined = self._chain()
        combined._merge_known_related_objects(other)
        combined.query.combine(other.query, sql.AND)
        return combined

    def __or__(self, other):
        from django import native as _native

        self._check_operator_queryset(other, "|")
        self._merge_sanity_check(other)
        if _native.AVAILABLE:
            kind = _native.qs_or_empty_kind(
                isinstance(self, EmptyQuerySet), isinstance(other, EmptyQuerySet)
            )
            if kind == 1:
                return other
            if kind == 2:
                return self
        else:
            if isinstance(self, EmptyQuerySet):
                return other
            if isinstance(other, EmptyQuerySet):
                return self
        query = (
            self
            if self.query.can_filter()
            else self.model._base_manager.filter(pk__in=self.values("pk"))
        )
        combined = query._chain()
        combined._merge_known_related_objects(other)
        if not other.query.can_filter():
            other = other.model._base_manager.filter(pk__in=other.values("pk"))
        combined.query.combine(other.query, sql.OR)
        return combined

    def __xor__(self, other):
        from django import native as _native

        self._check_operator_queryset(other, "^")
        self._merge_sanity_check(other)
        if _native.AVAILABLE:
            kind = _native.qs_or_empty_kind(
                isinstance(self, EmptyQuerySet), isinstance(other, EmptyQuerySet)
            )
            if kind == 1:
                return other
            if kind == 2:
                return self
        else:
            if isinstance(self, EmptyQuerySet):
                return other
            if isinstance(other, EmptyQuerySet):
                return self
        query = (
            self
            if self.query.can_filter()
            else self.model._base_manager.filter(pk__in=self.values("pk"))
        )
        combined = query._chain()
        combined._merge_known_related_objects(other)
        if not other.query.can_filter():
            other = other.model._base_manager.filter(pk__in=other.values("pk"))
        combined.query.combine(other.query, sql.XOR)
        return combined

    ####################################
    # METHODS THAT DO DATABASE QUERIES #
    ####################################

    def _iterator(self, use_chunked_fetch, chunk_size):
        iterable = self._iterable_class(
            self,
            chunked_fetch=use_chunked_fetch,
            chunk_size=chunk_size or 2000,
        )
        if not self._prefetch_related_lookups or chunk_size is None:
            yield from iterable
            return

        iterator = iter(iterable)
        while results := list(islice(iterator, chunk_size)):
            prefetch_related_objects(results, *self._prefetch_related_lookups)
            yield from results

    def iterator(self, chunk_size=None):
        """
        An iterator over the results from applying this QuerySet to the
        database. chunk_size must be provided for QuerySets that prefetch
        related objects. Otherwise, a default chunk_size of 2000 is supplied.
        """
        from django import native as _native

        if _native.AVAILABLE:
            code = _native.iterator_chunk_validate(
                chunk_size is None,
                0 if chunk_size is None else int(chunk_size),
                bool(self._prefetch_related_lookups),
            )
            if code == 1:
                raise ValueError(
                    "chunk_size must be provided when using QuerySet.iterator() after "
                    "prefetch_related()."
                )
            if code == 2:
                raise ValueError("Chunk size must be strictly positive.")
        else:
            if chunk_size is None:
                if self._prefetch_related_lookups:
                    raise ValueError(
                        "chunk_size must be provided when using QuerySet.iterator() after "
                        "prefetch_related()."
                    )
            elif chunk_size <= 0:
                raise ValueError("Chunk size must be strictly positive.")
        use_chunked_fetch = not connections[self.db].settings_dict.get(
            "DISABLE_SERVER_SIDE_CURSORS"
        )
        return self._iterator(use_chunked_fetch, chunk_size)

    async def aiterator(self, chunk_size=2000):
        """
        An asynchronous iterator over the results from applying this QuerySet
        to the database.
        """
        if chunk_size <= 0:
            raise ValueError("Chunk size must be strictly positive.")
        use_chunked_fetch = not connections[self.db].settings_dict.get(
            "DISABLE_SERVER_SIDE_CURSORS"
        )
        iterable = self._iterable_class(
            self, chunked_fetch=use_chunked_fetch, chunk_size=chunk_size
        )
        if self._prefetch_related_lookups:
            results = []

            async for item in iterable:
                results.append(item)
                if len(results) >= chunk_size:
                    await aprefetch_related_objects(
                        results, *self._prefetch_related_lookups
                    )
                    for result in results:
                        yield result
                    results.clear()

            if results:
                await aprefetch_related_objects(
                    results, *self._prefetch_related_lookups
                )
                for result in results:
                    yield result
        else:
            async for item in iterable:
                yield item

    def aggregate(self, *args, **kwargs):
        """
        Return a dictionary containing the calculations (aggregation)
        over the current queryset.

        If args is present the expression is passed as a kwarg using
        the Aggregate object's default alias.
        """
        from django import native as _native

        if _native.AVAILABLE:
            if _native.aggregate_distinct_fields_error(
                bool(self.query.distinct_fields)
            ):
                raise NotImplementedError(
                    "aggregate() + distinct(fields) not implemented."
                )
        elif self.query.distinct_fields:
            raise NotImplementedError("aggregate() + distinct(fields) not implemented.")
        self._validate_values_are_expressions(
            (*args, *kwargs.values()), method_name="aggregate"
        )
        for arg in args:
            # The default_alias property raises TypeError if default_alias
            # can't be set automatically or AttributeError if it isn't an
            # attribute.
            try:
                arg.default_alias
            except (AttributeError, TypeError):
                raise TypeError("Complex aggregates require an alias")
            kwargs[arg.default_alias] = arg

        native_agg = self._native_terminal_aggregate(kwargs)
        if native_agg is not _FAST_PATH_MISS:
            return native_agg
        return self.query.chain().get_aggregation(self.db, kwargs)

    async def aaggregate(self, *args, **kwargs):
        return await sync_to_async(self.aggregate)(*args, **kwargs)

    def _native_terminal_update(self, kwargs):
        if not kwargs:
            return _FAST_PATH_MISS
        connection = connections[self.db]
        for name, value in kwargs.items():
            if LOOKUP_SEP in name or hasattr(value, "resolve_expression"):
                return _FAST_PATH_MISS
        if self._native_qs is None and self._native_authoritative:
            try:
                from django.native import orm as native_orm

                compiled = native_orm.compile_query_plan_update(
                    self.model,
                    connection,
                    self._native_plan,
                    updates=kwargs,
                )
            except Exception:
                return _FAST_PATH_MISS
        else:
            handle = self._native_handle_for_sql()
            if handle is None:
                return _FAST_PATH_MISS
            try:
                compiled = handle.compile_update_kwargs(
                    kwargs, native_orm._parameter_types_for_connection(connection)
                )
            except Exception:
                return _FAST_PATH_MISS
        if compiled is None:
            return _FAST_PATH_MISS
        sql, params = compiled
        if not sql:
            return _FAST_PATH_MISS
        # Once execution starts, propagate database errors. Retrying the stock
        # compiler after an error can leave the surrounding transaction broken.
        with transaction.mark_for_rollback_on_error(using=self.db):
            with connection.cursor() as cursor:
                cursor.execute(sql, params)
                rowcount = cursor.rowcount
        if rowcount is None or rowcount < 0:
            return _FAST_PATH_MISS
        return rowcount

    def _native_terminal_aggregate(self, kwargs):
        """
        .aggregate(Count/Sum/Avg/Min/Max) via native handle when possible.
        """
        try:
            from django.native import orm as native_orm
            from django.db.models.aggregates import Aggregate
            from django.db.models.expressions import Star

            connection = connections[self.db]
            handle = self._native_handle_for_sql()
            if handle is None:
                if not self._simple_base_eligible(require_empty_where=True):
                    return _FAST_PATH_MISS
                handle = native_orm.build_queryset(self.model, connection)
                if handle is None:
                    return _FAST_PATH_MISS
            qs = handle.clone()
            qs.clear_select()
            aliases = []
            for alias, expr in kwargs.items():
                if not isinstance(expr, Aggregate):
                    return _FAST_PATH_MISS
                func = expr.function
                star = False
                field_name = ""
                distinct = bool(getattr(expr, "distinct", False))
                sources = expr.get_source_expressions()
                if not sources or isinstance(sources[0], Star):
                    star = True
                else:
                    src = sources[0]
                    name = getattr(src, "name", None)
                    if name is None and hasattr(src, "target"):
                        name = getattr(src.target, "name", None)
                    if isinstance(name, str):
                        field_name = name
                    else:
                        return _FAST_PATH_MISS
                if not qs.annotate_aggregate(alias, func, field_name, distinct, star):
                    return _FAST_PATH_MISS
                aliases.append(alias)
            sql, params = qs.compile_sql(
                native_orm._parameter_types_for_connection(connection)
            )
            if not sql:
                return _FAST_PATH_MISS
            row = native_orm.execute_fetchall(connection, sql, params)
            if not row:
                return {a: None for a in aliases}
            return dict(zip(aliases, row[0]))
        except Exception:
            return _FAST_PATH_MISS

    def count(self):
        """
        Perform a SELECT COUNT() and return the number of records as an
        integer.

        If the QuerySet is already fully cached, return the length of the
        cached results set to avoid multiple SELECT COUNT(*) calls.
        """
        from django import native as _native

        if _native.AVAILABLE:
            cached = _native.queryset_count_from_cache(
                self._result_cache is not None,
                len(self._result_cache) if self._result_cache is not None else 0,
            )
            if cached >= 0:
                return cached
        elif self._result_cache is not None:
            return len(self._result_cache)

        return self.query.get_count(using=self.db)

    async def acount(self):
        return await sync_to_async(self.count)()

    def get(self, *args, **kwargs):
        """
        Perform the query and return a single object matching the given
        keyword arguments.
        """
        if self._deferred_filter:
            # Related-manager core filters must be part of eligibility checks
            # and cannot be skipped by a native terminal.
            self.query
        combinator = self._query.combinator if self._query is not None else None
        if combinator and (args or kwargs):
            raise NotSupportedError(
                "Calling QuerySet.get(...) with filters after %s() is not "
                "supported." % combinator
            )
        # Tier 1+ fast paths: simple values_list/model get without full compiler.
        if not args and kwargs:
            fast = self._fast_path_simple_get(kwargs)
            if fast is not _FAST_PATH_MISS:
                return fast
        # Full native terminal: filter onto handle then fetch one.
        if args or kwargs:
            clone = self.filter(*args, **kwargs)
        else:
            clone = self._chain()
        native_result = clone._native_terminal_get()
        if native_result is not _FAST_PATH_MISS:
            return native_result
        if self.query.can_filter() and not clone.query.distinct_fields:
            clone = clone.order_by()
        limit = None
        if (
            not clone.query.select_for_update
            or connections[clone.db].features.supports_select_for_update_with_limit
        ):
            limit = MAX_GET_RESULTS
            clone.query.set_limits(high=limit)
        num = len(clone)
        if num == 1:
            return clone._result_cache[0]
        if not num:
            raise self.model.DoesNotExist(
                "%s matching query does not exist." % self.model._meta.object_name
            )
        raise self.model.MultipleObjectsReturned(
            "get() returned more than one %s -- it returned %s!"
            % (
                self.model._meta.object_name,
                num if not limit or num < limit else "more than %s" % (limit - 1),
            )
        )

    def _native_terminal_get(self):
        """get() via live native handle; returns object or _FAST_PATH_MISS."""
        handle = self._native_handle_for_sql()
        if handle is None:
            return _FAST_PATH_MISS
        if self._fields is not None:
            return _FAST_PATH_MISS  # values_list get uses other path
        try:
            from django.native import orm as native_orm

            connection = connections[self.db]
            handle = handle.clone()
            if not handle.set_ordering([]):
                return _FAST_PATH_MISS
            result = native_orm.materialize_models(
                self.model, connection, handle, limit=MAX_GET_RESULTS
            )
            if result is None:
                return _FAST_PATH_MISS
            objs, prefetches = result
            if not objs:
                raise self.model.DoesNotExist(
                    "%s matching query does not exist." % self.model._meta.object_name
                )
            if len(objs) > 1:
                raise self.model.MultipleObjectsReturned(
                    "get() returned more than one %s -- it returned %s!"
                    % (self.model._meta.object_name, len(objs))
                )
            obj = objs[0]
            if prefetches:
                self._native_run_prefetch([obj], prefetches)
            return obj
        except (self.model.DoesNotExist, self.model.MultipleObjectsReturned):
            raise
        except Exception:
            return _FAST_PATH_MISS

    def _native_run_prefetch(self, objs, lookups):
        """Run Django prefetch machinery for native-fetched instances."""
        if not objs or not lookups:
            return
        try:
            from django.db.models.query import prefetch_related_objects

            prefetch_related_objects(objs, *lookups)
        except Exception:
            pass

    def _native_terminal_fetch_all(self):
        """ModelIterable _fetch_all via native handle."""
        handle = self._native_handle_for_sql()
        if handle is None:
            return _FAST_PATH_MISS
        if self._fields is not None:
            return _FAST_PATH_MISS
        if not issubclass(self._iterable_class, ModelIterable):
            return _FAST_PATH_MISS
        try:
            from django.native import orm as native_orm

            connection = connections[self.db]
            h = handle.clone()
            query = self._query
            if query is not None and query.is_sliced:
                low = query.low_mark or 0
                high = query.high_mark
                if high is not None:
                    h.set_limit(int(high - low))
                if low:
                    h.set_offset(int(low))
            result = native_orm.materialize_models(self.model, connection, h)
            if result is None:
                return _FAST_PATH_MISS
            objs, prefetches = result
            if prefetches or self._prefetch_related_lookups:
                lookups = list(prefetches) + list(self._prefetch_related_lookups)
                self._native_run_prefetch(objs, lookups)
                self._prefetch_done = True
            return objs
        except Exception:
            return _FAST_PATH_MISS

    def _simple_base_eligible(self, *, require_empty_where=True):
        """Shared guards for simple single-table SELECT fast paths."""
        if self._native_authoritative and not self._native_has_only_projection():
            return False
        if self.model._meta.parents or self.model._meta.ordering:
            return False
        query = self._query
        if query is None:
            return True
        if (
            query.combinator
            or query.is_sliced
            or query.is_empty()
            or query.distinct
            or query.distinct_fields
            or query.select_for_update
            or query.select_related
            or query.group_by
            or query.annotations
            or query.extra
            or query.extra_order_by
            or query.deferred_loading != (frozenset(), True)
            or query._filtered_relations
            or len(query.alias_map) > 1
        ):
            return False
        if require_empty_where and query.where.children:
            return False
        return True

    def _resolve_lookup_field(self, lookup_name):
        opts = self.model._meta
        try:
            if lookup_name == "pk":
                return opts.pk
            return opts.get_field(lookup_name)
        except exceptions.FieldDoesNotExist:
            return None

    def _resolve_select_columns(self, field_names):
        """Return list of DB column names or None if any field is ineligible."""
        fields = self._resolve_select_fields(field_names)
        if fields is None:
            return None
        return [field.column for field in fields]

    def _resolve_select_fields(self, field_names):
        """Return concrete fields for a simple projection, or None."""
        opts = self.model._meta
        resolved = []
        for name in field_names:
            if not isinstance(name, str) or LOOKUP_SEP in name:
                return None
            try:
                f = opts.get_field(name)
            except exceptions.FieldDoesNotExist:
                return None
            if not getattr(f, "concrete", False) or not f.column:
                return None
            resolved.append(f)
        return tuple(resolved)

    def _concrete_field_ok(self, field):
        if field is None:
            return False
        if not getattr(field, "concrete", False) or field.many_to_many:
            return False
        if field.is_relation and not (field.many_to_one or field.one_to_one):
            return False
        return bool(field.column)

    def _fast_path_simple_get(self, kwargs):
        """
        Fast path for::

            Model.objects.values_list('a','b').get(col=value)
            Model.objects.get(pk=value)   # model instance

        when the queryset has no prior filters/joins/annotations.
        """
        simple_projection = (
            self._native_authoritative
            and self._native_qs is None
            and self._native_has_only_projection()
        )
        if len(kwargs) != 1 or (
            not simple_projection and not self._simple_base_eligible()
        ):
            return _FAST_PATH_MISS
        lookup_name, value = next(iter(kwargs.items()))
        if not isinstance(lookup_name, str) or LOOKUP_SEP in lookup_name:
            return _FAST_PATH_MISS

        # --- values_list / values tuple path ---
        if self._fields is not None and self._iterable_class in (
            ValuesListIterable,
            FlatValuesListIterable,
        ):
            if not self._fields:
                return _FAST_PATH_MISS
            return self._execute_simple_eq_get(
                self._fields, lookup_name, value, as_model=False
            )

        # --- model instance path ---
        if self._fields is None and issubclass(self._iterable_class, ModelIterable):
            opts = self.model._meta
            field_names = [f.attname for f in opts.concrete_fields if f.column]
            if not field_names:
                return _FAST_PATH_MISS
            return self._execute_simple_eq_get(
                field_names,
                lookup_name,
                value,
                as_model=True,
                model_field_names=field_names,
            )
        return _FAST_PATH_MISS

    def _execute_simple_eq_get(
        self, field_names, lookup_name, value, *, as_model, model_field_names=None
    ):
        db = self.db
        connection = connections[db]
        limit = MAX_GET_RESULTS
        sql = None
        params = ()
        try:
            from django.native import orm as native_orm

            compiled = native_orm.compile_values_list_get(
                self.model,
                field_names=field_names,
                lookup_field=lookup_name,
                lookup_value=value,
                limit=limit,
                connection=connection,
            )
            if compiled is not None:
                sql, param_list = compiled
                params = tuple(param_list)
        except Exception:
            sql = None

        if sql is None:
            return _FAST_PATH_MISS

        try:
            from django.native import orm as native_orm

            row, multi = native_orm.execute_fetchone_pair(connection, sql, params)
        except Exception:
            return _FAST_PATH_MISS

        if row is None:
            raise self.model.DoesNotExist(
                "%s matching query does not exist." % self.model._meta.object_name
            )
        if multi:
            raise self.model.MultipleObjectsReturned(
                "get() returned more than one %s -- it returned more than 1!"
                % self.model._meta.object_name
            )

        if as_model:
            return self.model.from_db(db, model_field_names, row)
        if self._iterable_class is FlatValuesListIterable:
            return row[0]
        return row

    def _native_compile_deferred_simple_values(self, connection):
        if (
            self._native_plan is None
            or self._native_qs is not None
            or not self._native_authoritative
            or self._fields is None
        ):
            return None
        from django.native import orm as native_orm

        return native_orm.compile_query_plan_values(
            self.model, connection, self._native_plan
        )

    def _fast_path_simple_values_fetch(self):
        """
        Fast path for values()/values_list() via C++ data plane.

        Prefers a live ``_native_qs`` handle (from filter/exclude wiring);
        otherwise builds a simple unfiltered select for fortune-shaped scans.
        """
        if self._fields is None:
            return _FAST_PATH_MISS
        if self._iterable_class not in (
            ValuesIterable,
            ValuesListIterable,
            FlatValuesListIterable,
        ):
            return _FAST_PATH_MISS
        if not self._native_authoritative and not self._resolve_select_columns(
            self._fields
        ):
            return _FAST_PATH_MISS

        db = self.db
        connection = connections[db]
        try:
            from django.native import orm as native_orm

            compiled = self._native_compile_deferred_simple_values(connection)
            if compiled is not None:
                sql, params = compiled
            else:
                handle = self._native_handle_for_sql()
                if handle is not None:
                    qs = handle
                    query = self._query
                    if query is not None and query.is_sliced:
                        # Respect high/low marks when set.
                        qs = handle.clone()
                        low = query.low_mark or 0
                        high = query.high_mark
                        if high is not None:
                            qs.set_limit(int(high - low))
                        if low:
                            qs.set_offset(int(low))
                    sql, params = qs.compile_sql(
                        native_orm._parameter_types_for_connection(connection)
                    )
                elif self._simple_base_eligible():
                    compiled = native_orm.compile_select(
                        self.model,
                        connection,
                        field_names=list(self._fields),
                        kwargs=None,
                    )
                    if compiled is None:
                        return _FAST_PATH_MISS
                    sql, params = compiled
                else:
                    return _FAST_PATH_MISS
            if not sql:
                return _FAST_PATH_MISS
            rows = native_orm.execute_fetchall(connection, sql, params)
        except Exception:
            return _FAST_PATH_MISS

        names = list(self._fields)
        if self._iterable_class is ValuesIterable:
            return [dict(zip(names, row)) for row in rows]
        if self._iterable_class is FlatValuesListIterable:
            return [row[0] for row in rows]
        return list(rows)

    def _fast_path_simple_filter_update(self, kwargs):
        """
        Fast path for::

            Model.objects.filter(pk=pk).update(field=value, ...)

        via C++ UPDATE compile when WHERE is a single exact lookup.
        """
        if not kwargs:
            return _FAST_PATH_MISS
        query = self.query
        if not self._simple_base_eligible(require_empty_where=False):
            return _FAST_PATH_MISS
        children = query.where.children
        if len(children) != 1:
            return _FAST_PATH_MISS
        child = children[0]
        lhs = getattr(child, "lhs", None)
        rhs = getattr(child, "rhs", None)
        lookup_name = getattr(child, "lookup_name", None)
        if lookup_name != "exact" or lhs is None:
            return _FAST_PATH_MISS
        if hasattr(rhs, "resolve_expression"):
            return _FAST_PATH_MISS
        target = getattr(lhs, "target", None) or getattr(lhs, "field", None)
        if not self._concrete_field_ok(target):
            return _FAST_PATH_MISS

        opts = self.model._meta
        update_kwargs = {}
        for name, value in kwargs.items():
            if not isinstance(name, str) or LOOKUP_SEP in name:
                return _FAST_PATH_MISS
            if hasattr(value, "resolve_expression"):
                return _FAST_PATH_MISS
            try:
                field = opts.get_field(name)
            except exceptions.FieldDoesNotExist:
                return _FAST_PATH_MISS
            if (
                not self._concrete_field_ok(field)
                or field.primary_key
                or field.generated
            ):
                return _FAST_PATH_MISS
            update_kwargs[name] = value

        db = self.db
        connection = connections[db]
        try:
            where_key = "pk" if target.primary_key else target.attname
            where_val = target.get_db_prep_value(rhs, connection, prepared=False)
            prep_updates = {}
            for name, value in update_kwargs.items():
                field = opts.get_field(name)
                prep_updates[name] = field.get_db_prep_save(value, connection)
            from django.native import orm as native_orm

            compiled = native_orm.compile_update(
                self.model,
                connection,
                filter_kwargs={where_key: where_val},
                update_kwargs=prep_updates,
            )
            if compiled is None:
                return _FAST_PATH_MISS
            sql, params = compiled
        except Exception:
            return _FAST_PATH_MISS

        with transaction.mark_for_rollback_on_error(using=db):
            with connection.cursor() as cursor:
                cursor.execute(sql, params)
                rowcount = cursor.rowcount
        if rowcount is None or rowcount < 0:
            return _FAST_PATH_MISS
        return rowcount

    def _fast_path_simple_in_values(self, field_name, id_list):
        """
        SELECT cols FROM t WHERE field IN (...) for values_list/values querysets.
        Via C++ data plane (filter_kwargs + compile + fetchall).
        """
        if not self._simple_base_eligible():
            return _FAST_PATH_MISS
        if self._fields is None:
            return _FAST_PATH_MISS
        if self._iterable_class not in (
            ValuesListIterable,
            FlatValuesListIterable,
            ValuesIterable,
        ):
            return _FAST_PATH_MISS
        if not self._resolve_select_columns(self._fields):
            return _FAST_PATH_MISS
        lookup_field = self._resolve_lookup_field(field_name)
        if not self._concrete_field_ok(lookup_field):
            return _FAST_PATH_MISS
        id_list = tuple(id_list)
        if not id_list:
            return []

        db = self.db
        connection = connections[db]
        try:
            params = [
                lookup_field.get_db_prep_value(v, connection, prepared=False)
                for v in id_list
            ]
            key = ("pk" if lookup_field.primary_key else lookup_field.attname) + "__in"
            from django.native import orm as native_orm

            compiled = native_orm.compile_select(
                self.model,
                connection,
                field_names=list(self._fields),
                kwargs={key: params},
            )
            if compiled is None:
                return _FAST_PATH_MISS
            sql, sql_params = compiled
            rows = native_orm.execute_fetchall(connection, sql, sql_params)
        except Exception:
            return _FAST_PATH_MISS

        if self._iterable_class is ValuesIterable:
            names = list(self._fields)
            return [dict(zip(names, row)) for row in rows]
        if self._iterable_class is FlatValuesListIterable:
            return [row[0] for row in rows]
        return list(rows)

    async def aget(self, *args, **kwargs):
        return await sync_to_async(self.get)(*args, **kwargs)

    def create(self, **kwargs):
        """
        Create a new object with the given kwargs, saving it to the database
        and returning the created object.
        """
        from django import native as _native

        reverse_one_to_one_fields = frozenset(kwargs).intersection(
            self.model._meta._reverse_one_to_one_field_names
        )
        if reverse_one_to_one_fields:
            if _native.AVAILABLE:
                names = _native.join_sorted_comma(list(reverse_one_to_one_fields))
            else:
                names = ", ".join(reverse_one_to_one_fields)
            raise ValueError(
                "The following fields do not exist in this model: %s" % names
            )

        obj = self.model(**kwargs)
        self._for_write = True
        obj.save(force_insert=True, using=self.db)
        return obj

    create.alters_data = True

    async def acreate(self, **kwargs):
        return await sync_to_async(self.create)(**kwargs)

    acreate.alters_data = True

    def _prepare_for_bulk_create(self, objs):
        from django import native as _native

        if _native.AVAILABLE and isinstance(objs, (list, tuple)):
            if _native.collector_add_empty(len(objs)):
                return [], []
        objs_with_pk, objs_without_pk = [], []
        for obj in objs:
            if isinstance(obj.pk, DatabaseDefault):
                objs_without_pk.append(obj)
            elif obj._is_pk_set():
                objs_with_pk.append(obj)
            else:
                obj.pk = obj._meta.pk.get_pk_value_on_save(obj)
                if obj._is_pk_set():
                    objs_with_pk.append(obj)
                else:
                    objs_without_pk.append(obj)
            obj._prepare_related_fields_for_save(operation_name="bulk_create")
        return objs_with_pk, objs_without_pk

    def _check_bulk_create_options(
        self, ignore_conflicts, update_conflicts, update_fields, unique_fields
    ):
        from django import native as _native

        if _native.AVAILABLE:
            kind = _native.bulk_create_conflict_kind(
                bool(ignore_conflicts), bool(update_conflicts)
            )
            if kind == -1:
                raise ValueError(
                    "ignore_conflicts and update_conflicts are mutually exclusive."
                )
        elif ignore_conflicts and update_conflicts:
            raise ValueError(
                "ignore_conflicts and update_conflicts are mutually exclusive."
            )

        db_features = connections[self.db].features
        if _native.AVAILABLE:
            if kind == 1:
                if not db_features.supports_ignore_conflicts:
                    raise NotSupportedError(
                        "This database backend does not support ignoring conflicts."
                    )
                return OnConflict.IGNORE
            if kind == 2:
                if not db_features.supports_update_conflicts:
                    raise NotSupportedError(
                        "This database backend does not support updating conflicts."
                    )
                if not update_fields:
                    raise ValueError(
                        "Fields that will be updated when a row insertion fails "
                        "on conflicts must be provided."
                    )
                if (
                    unique_fields
                    and not db_features.supports_update_conflicts_with_target
                ):
                    raise NotSupportedError(
                        "This database backend does not support updating "
                        "conflicts with specifying unique fields that can trigger "
                        "the upsert."
                    )
                if (
                    not unique_fields
                    and db_features.supports_update_conflicts_with_target
                ):
                    raise ValueError(
                        "Unique fields that can trigger the upsert must be provided."
                    )
                if any(not f.concrete for f in update_fields):
                    raise ValueError(
                        "bulk_create() can only be used with concrete fields in "
                        "update_fields."
                    )
                if any(f in self.model._meta.pk_fields for f in update_fields):
                    raise ValueError(
                        "bulk_create() cannot be used with primary keys in "
                        "update_fields."
                    )
                if unique_fields:
                    if any(not f.concrete for f in unique_fields):
                        raise ValueError(
                            "bulk_create() can only be used with concrete fields "
                            "in unique_fields."
                        )
                return OnConflict.UPDATE
            return None

        if ignore_conflicts:
            if not db_features.supports_ignore_conflicts:
                raise NotSupportedError(
                    "This database backend does not support ignoring conflicts."
                )
            return OnConflict.IGNORE
        elif update_conflicts:
            if not db_features.supports_update_conflicts:
                raise NotSupportedError(
                    "This database backend does not support updating conflicts."
                )
            if not update_fields:
                raise ValueError(
                    "Fields that will be updated when a row insertion fails "
                    "on conflicts must be provided."
                )
            if unique_fields and not db_features.supports_update_conflicts_with_target:
                raise NotSupportedError(
                    "This database backend does not support updating "
                    "conflicts with specifying unique fields that can trigger "
                    "the upsert."
                )
            if not unique_fields and db_features.supports_update_conflicts_with_target:
                raise ValueError(
                    "Unique fields that can trigger the upsert must be provided."
                )
            # Updating primary keys and non-concrete fields is forbidden.
            if any(not f.concrete for f in update_fields):
                raise ValueError(
                    "bulk_create() can only be used with concrete fields in "
                    "update_fields."
                )
            if any(f in self.model._meta.pk_fields for f in update_fields):
                raise ValueError(
                    "bulk_create() cannot be used with primary keys in "
                    "update_fields."
                )
            if unique_fields:
                if any(not f.concrete for f in unique_fields):
                    raise ValueError(
                        "bulk_create() can only be used with concrete fields "
                        "in unique_fields."
                    )
            return OnConflict.UPDATE
        return None

    def bulk_create(
        self,
        objs,
        batch_size=None,
        ignore_conflicts=False,
        update_conflicts=False,
        update_fields=None,
        unique_fields=None,
    ):
        """
        Insert each of the instances into the database. Do *not* call
        save() on each of the instances, do not send any pre/post_save
        signals, and do not set the primary key attribute if it is an
        autoincrement field (except if
        features.can_return_rows_from_bulk_insert=True).
        Multi-table models are not supported.
        """
        # When you bulk insert you don't get the primary keys back (if it's an
        # autoincrement, except if can_return_rows_from_bulk_insert=True), so
        # you can't insert into the child tables which references this. There
        # are two workarounds:
        # 1) This could be implemented if you didn't have an autoincrement pk
        # 2) You could do it by doing O(n) normal inserts into the parent
        #    tables to get the primary keys back and then doing a single bulk
        #    insert into the childmost table.
        # We currently set the primary keys on the objects when using
        # PostgreSQL via the RETURNING ID clause. It should be possible for
        # Oracle as well, but the semantics for extracting the primary keys is
        # trickier so it's not done yet.
        from django import native as _native

        if _native.AVAILABLE:
            if _native.validate_positive_batch_size(
                batch_size is None, 0 if batch_size is None else int(batch_size)
            ):
                raise ValueError("Batch size must be a positive integer.")
        elif batch_size is not None and batch_size <= 0:
            raise ValueError("Batch size must be a positive integer.")
        # Check that the parents share the same concrete model with the our
        # model to detect the inheritance pattern ConcreteGrandParent ->
        # MultiTableParent -> ProxyChild. Simply checking
        # self.model._meta.proxy would not identify that case as involving
        # multiple tables.
        for parent in self.model._meta.all_parents:
            if parent._meta.concrete_model is not self.model._meta.concrete_model:
                raise ValueError("Can't bulk create a multi-table inherited model")
        if not objs:
            return objs
        opts = self.model._meta
        if unique_fields:
            # Primary key is allowed in unique_fields.
            unique_fields = [
                self.model._meta.get_field(opts.pk.name if name == "pk" else name)
                for name in unique_fields
            ]
        if update_fields:
            update_fields = [self.model._meta.get_field(name) for name in update_fields]
        on_conflict = self._check_bulk_create_options(
            ignore_conflicts,
            update_conflicts,
            update_fields,
            unique_fields,
        )
        self._for_write = True
        fields = [f for f in opts.concrete_fields if not f.generated]
        objs = list(objs)
        objs_with_pk, objs_without_pk = self._prepare_for_bulk_create(objs)
        if objs_with_pk and objs_without_pk:
            context = transaction.atomic(using=self.db, savepoint=False)
        else:
            context = nullcontext()
        with context:
            self._handle_order_with_respect_to(objs)
            if objs_with_pk:
                returned_columns = self._batched_insert(
                    objs_with_pk,
                    fields,
                    batch_size,
                    on_conflict=on_conflict,
                    update_fields=update_fields,
                    unique_fields=unique_fields,
                )
                for obj_with_pk, results in zip(objs_with_pk, returned_columns):
                    for result, field in zip(results, opts.db_returning_fields):
                        if field != opts.pk:
                            setattr(obj_with_pk, field.attname, result)
                for obj_with_pk in objs_with_pk:
                    obj_with_pk._state.adding = False
                    obj_with_pk._state.db = self.db
            if objs_without_pk:
                fields = [f for f in fields if not isinstance(f, AutoField)]
                returned_columns = self._batched_insert(
                    objs_without_pk,
                    fields,
                    batch_size,
                    on_conflict=on_conflict,
                    update_fields=update_fields,
                    unique_fields=unique_fields,
                )
                connection = connections[self.db]
                if (
                    connection.features.can_return_rows_from_bulk_insert
                    and on_conflict is None
                ):
                    assert len(returned_columns) == len(objs_without_pk)
                for obj_without_pk, results in zip(objs_without_pk, returned_columns):
                    for result, field in zip(results, opts.db_returning_fields):
                        setattr(obj_without_pk, field.attname, result)
                    obj_without_pk._state.adding = False
                    obj_without_pk._state.db = self.db

        return objs

    def _handle_order_with_respect_to(self, objs):
        if objs and (order_wrt := self.model._meta.order_with_respect_to):
            get_filter_kwargs_for_object = order_wrt.get_filter_kwargs_for_object
            attnames = list(get_filter_kwargs_for_object(objs[0]))
            group_keys = set()
            obj_groups = []
            for obj in objs:
                group_key = tuple(get_filter_kwargs_for_object(obj).values())
                group_keys.add(group_key)
                obj_groups.append((obj, group_key))
            filters = [
                Q.create(list(zip(attnames, group_key))) for group_key in group_keys
            ]
            next_orders = (
                self.model._base_manager.using(self.db)
                .filter(reduce(operator.or_, filters))
                .values_list(*attnames)
                .annotate(_order__max=Max("_order") + 1)
            )
            # Create mapping of group values to max order.
            group_next_orders = dict.fromkeys(group_keys, 0)
            group_next_orders.update(
                (tuple(group_key), next_order) for *group_key, next_order in next_orders
            )
            # Assign _order values to new objects.
            for obj, group_key in obj_groups:
                if getattr(obj, "_order", None) is None:
                    group_next_order = group_next_orders[group_key]
                    obj._order = group_next_order
                    group_next_orders[group_key] += 1

    bulk_create.alters_data = True

    async def abulk_create(
        self,
        objs,
        batch_size=None,
        ignore_conflicts=False,
        update_conflicts=False,
        update_fields=None,
        unique_fields=None,
    ):
        return await sync_to_async(self.bulk_create)(
            objs=objs,
            batch_size=batch_size,
            ignore_conflicts=ignore_conflicts,
            update_conflicts=update_conflicts,
            update_fields=update_fields,
            unique_fields=unique_fields,
        )

    abulk_create.alters_data = True

    def bulk_update(self, objs, fields, batch_size=None):
        """
        Update the given fields in each of the given objects in the database.
        """
        from django import native as _native

        if _native.AVAILABLE:
            if _native.validate_positive_batch_size(
                batch_size is None, 0 if batch_size is None else int(batch_size)
            ):
                raise ValueError("Batch size must be a positive integer.")
        elif batch_size is not None and batch_size <= 0:
            raise ValueError("Batch size must be a positive integer.")
        if not fields:
            raise ValueError("Field names must be given to bulk_update().")
        objs = tuple(objs)
        if not all(obj._is_pk_set() for obj in objs):
            raise ValueError("All bulk_update() objects must have a primary key set.")
        opts = self.model._meta
        fields = [opts.get_field(name) for name in fields]
        if any(not f.concrete for f in fields):
            raise ValueError("bulk_update() can only be used with concrete fields.")
        all_pk_fields = set(opts.pk_fields)
        for parent in opts.all_parents:
            all_pk_fields.update(parent._meta.pk_fields)
        if any(f in all_pk_fields for f in fields):
            raise ValueError("bulk_update() cannot be used with primary key fields.")
        if not objs:
            return 0
        for obj in objs:
            obj._prepare_related_fields_for_save(
                operation_name="bulk_update", fields=fields
            )
        # PK is used twice in the resulting update query, once in the filter
        # and once in the WHEN. Each field will also have one CAST.
        self._for_write = True
        connection = connections[self.db]
        max_batch_size = connection.ops.bulk_batch_size(
            [opts.pk, opts.pk, *fields], objs
        )
        if _native.AVAILABLE:
            batch_size = _native.effective_batch_size(
                batch_size is not None,
                0 if batch_size is None else int(batch_size),
                max_batch_size or 0,
                len(objs),
            )
        else:
            batch_size = (
                min(batch_size, max_batch_size) if batch_size else max_batch_size
            )
        requires_casting = connection.features.requires_casted_case_in_updates
        if _native.AVAILABLE:
            batches = (
                objs[start:end]
                for start, end in _native.in_bulk_batch_ranges(len(objs), batch_size)
            )
        else:
            batches = (
                objs[i : i + batch_size] for i in range(0, len(objs), batch_size)
            )
        updates = []
        for batch_objs in batches:
            update_kwargs = {}
            for field in fields:
                when_statements = []
                for obj in batch_objs:
                    attr = getattr(obj, field.attname)
                    if not hasattr(attr, "resolve_expression"):
                        attr = Value(attr, output_field=field)
                    when_statements.append(When(pk=obj.pk, then=attr))
                case_statement = Case(*when_statements, output_field=field)
                if requires_casting:
                    case_statement = Cast(case_statement, output_field=field)
                update_kwargs[field.attname] = case_statement
            updates.append(([obj.pk for obj in batch_objs], update_kwargs))
        rows_updated = 0
        queryset = self.using(self.db)
        with transaction.atomic(using=self.db, savepoint=False):
            for pks, update_kwargs in updates:
                rows_updated += queryset.filter(pk__in=pks).update(**update_kwargs)
        return rows_updated

    bulk_update.alters_data = True

    async def abulk_update(self, objs, fields, batch_size=None):
        return await sync_to_async(self.bulk_update)(
            objs=objs,
            fields=fields,
            batch_size=batch_size,
        )

    abulk_update.alters_data = True

    def get_or_create(self, defaults=None, **kwargs):
        """
        Look up an object with the given kwargs, creating one if necessary.
        Return a tuple of (object, created), where created is a boolean
        specifying whether an object was created.
        """
        # The get() needs to be targeted at the write database in order
        # to avoid potential transaction consistency problems.
        self._for_write = True
        try:
            return self.get(**kwargs), False
        except self.model.DoesNotExist:
            params = self._extract_model_params(defaults, **kwargs)
            # Try to create an object using passed params.
            try:
                with transaction.atomic(using=self.db):
                    params = dict(resolve_callables(params))
                    return self.create(**params), True
            except IntegrityError:
                try:
                    return self.get(**kwargs), False
                except self.model.DoesNotExist:
                    pass
                raise

    get_or_create.alters_data = True

    async def aget_or_create(self, defaults=None, **kwargs):
        return await sync_to_async(self.get_or_create)(
            defaults=defaults,
            **kwargs,
        )

    aget_or_create.alters_data = True

    def update_or_create(self, defaults=None, create_defaults=None, **kwargs):
        """
        Look up an object with the given kwargs, updating one with defaults
        if it exists, otherwise create a new one. Optionally, an object can
        be created with different values than defaults by using
        create_defaults.
        Return a tuple (object, created), where created is a boolean
        specifying whether an object was created.
        """
        from django import native as _native

        if _native.AVAILABLE:
            update_defaults = {} if defaults is None else defaults
            if _native.create_defaults_use_update(create_defaults is None):
                create_defaults = update_defaults
        else:
            update_defaults = defaults or {}
            if create_defaults is None:
                create_defaults = update_defaults

        self._for_write = True
        with transaction.atomic(using=self.db):
            # Lock the row so that a concurrent update is blocked until
            # update_or_create() has performed its save.
            obj, created = self.select_for_update().get_or_create(
                create_defaults, **kwargs
            )
            if created:
                return obj, created
            for k, v in resolve_callables(update_defaults):
                setattr(obj, k, v)

            update_fields = set(update_defaults)
            concrete_field_names = self.model._meta._non_pk_concrete_field_names
            # update_fields does not support non-concrete fields.
            if concrete_field_names.issuperset(update_fields):
                # Add fields which are set on pre_save(), e.g. auto_now fields.
                # This is to maintain backward compatibility as these fields
                # are not updated unless explicitly specified in the
                # update_fields list.
                pk_fields = self.model._meta.pk_fields
                for field in self.model._meta.local_concrete_fields:
                    if not (
                        field in pk_fields or field.__class__.pre_save is Field.pre_save
                    ):
                        update_fields.add(field.name)
                        if field.name != field.attname:
                            update_fields.add(field.attname)
                obj.save(using=self.db, update_fields=update_fields)
            else:
                obj.save(using=self.db)
        return obj, False

    update_or_create.alters_data = True

    async def aupdate_or_create(self, defaults=None, create_defaults=None, **kwargs):
        return await sync_to_async(self.update_or_create)(
            defaults=defaults,
            create_defaults=create_defaults,
            **kwargs,
        )

    aupdate_or_create.alters_data = True

    def _extract_model_params(self, defaults, **kwargs):
        """
        Prepare `params` for creating a model instance based on the given
        kwargs; for use by get_or_create().
        """
        from django import native as _native

        defaults = defaults or {}
        if _native.AVAILABLE:
            keep = _native.keys_without_lookup_sep(list(kwargs.keys()))
            params = {k: kwargs[k] for k in keep}
        else:
            params = {k: v for k, v in kwargs.items() if LOOKUP_SEP not in k}
        params.update(defaults)
        property_names = self.model._meta._property_names
        invalid_params = []
        for param in params:
            try:
                self.model._meta.get_field(param)
            except exceptions.FieldDoesNotExist:
                # It's okay to use a model's property if it has a setter.
                if not (param in property_names and getattr(self.model, param).fset):
                    invalid_params.append(param)
        if invalid_params:
            raise exceptions.FieldError(
                "Invalid field name(s) for model %s: '%s'."
                % (
                    self.model._meta.object_name,
                    "', '".join(sorted(invalid_params)),
                )
            )
        return params

    def _earliest(self, *fields):
        """
        Return the earliest object according to fields (if given) or by the
        model's Meta.get_latest_by.
        """
        from django import native as _native

        if fields:
            order_by = fields
        else:
            order_by = getattr(self.model._meta, "get_latest_by")
            if order_by and not isinstance(order_by, (tuple, list)):
                order_by = (order_by,)
        if _native.AVAILABLE:
            if _native.earliest_missing_fields(bool(fields), order_by is not None):
                raise ValueError(
                    "earliest() and latest() require either fields as positional "
                    "arguments or 'get_latest_by' in the model's Meta."
                )
        elif order_by is None:
            raise ValueError(
                "earliest() and latest() require either fields as positional "
                "arguments or 'get_latest_by' in the model's Meta."
            )
        obj = self._chain()
        obj.query.set_limits(high=1)
        obj.query.clear_ordering(force=True)
        obj.query.add_ordering(*order_by)
        return obj.get()

    def earliest(self, *fields):
        from django import native as _native

        if _native.AVAILABLE:
            if _native.queryset_sliced_error(self.query.is_sliced):
                raise TypeError("Cannot change a query once a slice has been taken.")
        elif self.query.is_sliced:
            raise TypeError("Cannot change a query once a slice has been taken.")
        return self._earliest(*fields)

    async def aearliest(self, *fields):
        return await sync_to_async(self.earliest)(*fields)

    def latest(self, *fields):
        """
        Return the latest object according to fields (if given) or by the
        model's Meta.get_latest_by.
        """
        from django import native as _native

        if _native.AVAILABLE:
            if _native.queryset_sliced_error(self.query.is_sliced):
                raise TypeError("Cannot change a query once a slice has been taken.")
        elif self.query.is_sliced:
            raise TypeError("Cannot change a query once a slice has been taken.")
        return self.reverse()._earliest(*fields)

    async def alatest(self, *fields):
        return await sync_to_async(self.latest)(*fields)

    def first(self):
        """Return the first object of a query or None if no match is found."""
        from django import native as _native

        if _native.AVAILABLE and _native.queryset_use_cache_for_first_last(
            self._result_cache is not None,
            self.ordered,
            bool(self._result_cache),
        ):
            return self._result_cache[0] if self._result_cache else None
        if self.ordered:
            queryset = self
        else:
            self._check_ordering_first_last_queryset_aggregation(method="first")
            queryset = self.order_by("pk")
        for obj in queryset[:1]:
            return obj

    async def afirst(self):
        return await sync_to_async(self.first)()

    def last(self):
        """Return the last object of a query or None if no match is found."""
        from django import native as _native

        if _native.AVAILABLE and _native.queryset_use_cache_for_first_last(
            self._result_cache is not None,
            self.ordered,
            bool(self._result_cache),
        ):
            return self._result_cache[-1] if self._result_cache else None
        if self.ordered:
            queryset = self.reverse()
        else:
            self._check_ordering_first_last_queryset_aggregation(method="last")
            queryset = self.order_by("-pk")
        for obj in queryset[:1]:
            return obj

    async def alast(self):
        return await sync_to_async(self.last)()

    def in_bulk(self, id_list=None, *, field_name="pk"):
        """
        Return a dictionary mapping each of the given IDs to the object with
        that ID. If `id_list` isn't provided, evaluate the entire QuerySet.
        """
        if self.query.is_sliced:
            raise TypeError("Cannot use 'limit' or 'offset' with in_bulk().")
        if not issubclass(self._iterable_class, ModelIterable):
            raise TypeError("in_bulk() cannot be used with values() or values_list().")
        opts = self.model._meta
        unique_fields = [
            constraint.fields[0]
            for constraint in opts.total_unique_constraints
            if len(constraint.fields) == 1
        ]
        if (
            field_name != "pk"
            and not opts.get_field(field_name).unique
            and field_name not in unique_fields
            and self.query.distinct_fields != (field_name,)
        ):
            raise ValueError(
                "in_bulk()'s field_name must be a unique field but %r isn't."
                % field_name
            )
        if id_list is not None:
            # Materialize first: id_list may be a one-shot iterator.
            id_list = tuple(id_list)
            if not id_list:
                return {}
            filter_key = "{}__in".format(field_name)
            # Fast path: unfiltered single-table IN query → model instances.
            if self._simple_base_eligible():
                lookup_field = self._resolve_lookup_field(field_name)
                if self._concrete_field_ok(lookup_field):
                    select_columns = [
                        f.column for f in opts.concrete_fields if f.column
                    ]
                    field_names = [f.attname for f in opts.concrete_fields if f.column]
                    if select_columns:
                        db = self.db
                        connection = connections[db]
                        n = len(id_list)
                        batch_size = connection.ops.bulk_batch_size([opts.pk], id_list)
                        result = {}
                        try:
                            from django.native import orm as native_orm
                        except Exception:
                            native_orm = None

                        def fetch_batch(batch):
                            params = [
                                lookup_field.get_db_prep_value(
                                    v, connection, prepared=False
                                )
                                for v in batch
                            ]
                            key = (
                                "pk"
                                if field_name == "pk" or lookup_field.primary_key
                                else lookup_field.attname
                            ) + "__in"
                            if native_orm is None:
                                return False
                            compiled = native_orm.compile_select(
                                self.model,
                                connection,
                                field_names=field_names,
                                kwargs={key: params},
                            )
                            if compiled is None:
                                return False
                            sql, sql_params = compiled
                            for row in native_orm.execute_fetchall(
                                connection, sql, sql_params
                            ):
                                obj = self.model.from_db(db, field_names, row)
                                result[
                                    getattr(
                                        obj,
                                        field_name if field_name != "pk" else "pk",
                                    )
                                ] = obj
                            return True

                        ok = True
                        if batch_size and batch_size < n:
                            for offset in range(0, n, batch_size):
                                if not fetch_batch(
                                    id_list[offset : offset + batch_size]
                                ):
                                    ok = False
                                    break
                        else:
                            ok = fetch_batch(id_list)
                        if ok:
                            return result
            batch_size = connections[self.db].ops.bulk_batch_size([opts.pk], id_list)
            # If the database has a limit on the number of query parameters
            # (e.g. SQLite), retrieve objects in batches if necessary.
            if batch_size and batch_size < len(id_list):
                qs = ()
                for offset in range(0, len(id_list), batch_size):
                    batch = id_list[offset : offset + batch_size]
                    qs += tuple(self.filter(**{filter_key: batch}))
            else:
                qs = self.filter(**{filter_key: id_list})
        else:
            qs = self._chain()
        return {getattr(obj, field_name): obj for obj in qs}

    async def ain_bulk(self, id_list=None, *, field_name="pk"):
        return await sync_to_async(self.in_bulk)(
            id_list=id_list,
            field_name=field_name,
        )

    def delete(self):
        """Delete the records in the current QuerySet."""
        from django import native as _native

        if _native.AVAILABLE:
            code = _native.queryset_write_guard(
                bool(self.query.combinator),
                self.query.is_sliced,
                bool(self.query.distinct_fields),
                self._fields is not None,
            )
            if code == 1:
                self._not_support_combined_queries("delete")
            elif code == 2:
                raise TypeError("Cannot use 'limit' or 'offset' with delete().")
            elif code == 3:
                raise TypeError("Cannot call delete() after .distinct(*fields).")
            elif code == 4:
                raise TypeError(
                    "Cannot call delete() after .values() or .values_list()"
                )
        else:
            self._not_support_combined_queries("delete")
            if self.query.is_sliced:
                raise TypeError("Cannot use 'limit' or 'offset' with delete().")
            if self.query.distinct_fields:
                raise TypeError("Cannot call delete() after .distinct(*fields).")
            if self._fields is not None:
                raise TypeError(
                    "Cannot call delete() after .values() or .values_list()"
                )

        # Empty queryset (e.g. pk__in=[]) — nothing to collect or delete.
        if _native.AVAILABLE and self.query.is_empty():
            self._result_cache = None
            return 0, {}

        del_query = self._chain()

        # The delete is actually 2 queries - one to find related objects,
        # and one to delete. Make sure that the discovery of related
        # objects is performed on the same database as the deletion.
        del_query._for_write = True

        # Disable non-supported fields.
        del_query.query.select_for_update = False
        del_query.query.select_related = False
        del_query.query.clear_ordering(force=True)

        collector = Collector(using=del_query.db, origin=self)
        collector.collect(del_query)
        num_deleted, num_deleted_per_model = collector.delete()

        # Clear the result cache, in case this QuerySet gets reused.
        self._result_cache = None
        return num_deleted, num_deleted_per_model

    delete.alters_data = True
    delete.queryset_only = True

    async def adelete(self):
        return await sync_to_async(self.delete)()

    adelete.alters_data = True
    adelete.queryset_only = True

    def _raw_delete(self, using):
        """
        Delete objects found from the given queryset in single direct SQL
        query. No signals are sent and there is no protection for cascades.
        """
        from django import native as _native

        if _native.AVAILABLE and self.query.is_empty():
            return 0
        query = self.query.clone()
        query.__class__ = sql.DeleteQuery
        return query.get_compiler(using).execute_sql(ROW_COUNT)

    _raw_delete.alters_data = True

    def update(self, **kwargs):
        """
        Update all elements in the current QuerySet, setting all the given
        fields to the appropriate values.
        """
        query_state = self._query
        if query_state is not None and query_state.combinator:
            self._not_support_combined_queries("update")
        if query_state is not None and query_state.is_sliced:
            raise TypeError("Cannot update a query once a slice has been taken.")
        self._for_write = True
        # An authoritative native filter compiles all assignments in one hop.
        native_upd = self._native_terminal_update(kwargs)
        if native_upd is not _FAST_PATH_MISS:
            self._result_cache = None
            return native_upd
        # Legacy mirrored single-row exact-lookup fast path.
        fast = self._fast_path_simple_filter_update(kwargs)
        if fast is not _FAST_PATH_MISS:
            self._result_cache = None
            return fast
        query = self.query.chain(sql.UpdateQuery)
        query.add_update_values(kwargs)

        # Inline annotations in order_by(), if possible.
        new_order_by = []
        for col in query.order_by:
            alias = col
            descending = False
            if isinstance(alias, str) and alias.startswith("-"):
                alias = alias.removeprefix("-")
                descending = True
            if annotation := query.annotations.get(alias):
                if getattr(annotation, "contains_aggregate", False):
                    raise exceptions.FieldError(
                        f"Cannot update when ordering by an aggregate: {annotation}"
                    )
                if descending:
                    annotation = annotation.desc()
                new_order_by.append(annotation)
            else:
                new_order_by.append(col)
        query.order_by = tuple(new_order_by)

        # Clear SELECT clause as all annotation references were inlined by
        # add_update_values() already.
        query.clear_select_clause()
        with transaction.mark_for_rollback_on_error(using=self.db):
            rows = query.get_compiler(self.db).execute_sql(ROW_COUNT)
        self._result_cache = None
        return rows

    update.alters_data = True

    async def aupdate(self, **kwargs):
        return await sync_to_async(self.update)(**kwargs)

    aupdate.alters_data = True

    def _update(self, values, returning_fields=None):
        """
        A version of update() that accepts field objects instead of field
        names. Used primarily for model saving and not intended for use by
        general code (it requires too much poking around at model internals to
        be useful at that level).
        """
        if self.query.is_sliced:
            raise TypeError("Cannot update a query once a slice has been taken.")
        query = self.query.chain(sql.UpdateQuery)
        query.add_update_fields(values)
        # Clear any annotations so that they won't be present in subqueries.
        query.annotations = {}
        self._result_cache = None
        if returning_fields is None:
            return query.get_compiler(self.db).execute_sql(ROW_COUNT)
        return query.get_compiler(self.db).execute_returning_sql(returning_fields)

    _update.alters_data = True
    _update.queryset_only = False

    def exists(self):
        """
        Return True if the QuerySet would have any results, False otherwise.
        """
        from django import native as _native

        if self._result_cache is None:
            return self.query.has_results(using=self.db)
        if _native.AVAILABLE:
            return _native.queryset_exists_from_cache(True, bool(self._result_cache))
        return bool(self._result_cache)

    async def aexists(self):
        return await sync_to_async(self.exists)()

    def contains(self, obj):
        """
        Return True if the QuerySet contains the provided obj,
        False otherwise.
        """
        from django import native as _native

        self._not_support_combined_queries("contains")
        try:
            if obj._meta.concrete_model != self.model._meta.concrete_model:
                return False
        except AttributeError:
            raise TypeError("'obj' must be a model instance.")
        if _native.AVAILABLE:
            code = _native.contains_preflight(
                self._fields is not None, obj._is_pk_set()
            )
            if code == 1:
                raise TypeError(
                    "Cannot call QuerySet.contains() after .values() or .values_list()."
                )
            if code == 2:
                raise ValueError(
                    "QuerySet.contains() cannot be used on unsaved objects."
                )
        else:
            if self._fields is not None:
                raise TypeError(
                    "Cannot call QuerySet.contains() after .values() or .values_list()."
                )
            if not obj._is_pk_set():
                raise ValueError(
                    "QuerySet.contains() cannot be used on unsaved objects."
                )
        if self._result_cache is not None:
            return obj in self._result_cache
        return self.filter(pk=obj.pk).exists()

    async def acontains(self, obj):
        return await sync_to_async(self.contains)(obj=obj)

    def _prefetch_related_objects(self):
        # This method can only be called once the result cache has been filled.
        prefetch_related_objects(self._result_cache, *self._prefetch_related_lookups)
        self._prefetch_done = True

    def explain(self, *, format=None, **options):
        """
        Runs an EXPLAIN on the SQL query this QuerySet would perform, and
        returns the results.
        """
        from django import native as _native

        using = self.db
        if _native.AVAILABLE:
            # Dual-path: resolve using via same db property (no extra logic).
            pass
        return self.query.explain(using=using, format=format, **options)

    async def aexplain(self, *, format=None, **options):
        return await sync_to_async(self.explain)(format=format, **options)

    ##################################################
    # PUBLIC METHODS THAT RETURN A QUERYSET SUBCLASS #
    ##################################################

    def raw(self, raw_query, params=(), translations=None, using=None):
        from django import native as _native

        if _native.AVAILABLE:
            if _native.using_is_none(using is None):
                using = self.db
        elif using is None:
            using = self.db
        qs = RawQuerySet(
            raw_query,
            model=self.model,
            params=params,
            translations=translations,
            using=using,
        )
        qs._prefetch_related_lookups = self._prefetch_related_lookups[:]
        return qs

    def _values(self, *fields, **expressions):
        clone = self._chain()
        if expressions:
            # RemovedInDjango70Warning: When the deprecation ends, deindent as:
            # clone = clone.annotate(**expressions)
            with warnings.catch_warnings(
                action="ignore", category=RemovedInDjango70Warning
            ):
                clone = clone.annotate(**expressions)
        clone._fields = fields
        clone.query.set_values(fields)
        return clone

    def _native_simple_values_projection(self, fields, iterable_class):
        """Keep a concrete-field projection in the authoritative native plan."""
        from django import native as _native

        if (
            not _native.AVAILABLE
            or not fields
            or not all(isinstance(field, str) for field in fields)
            or len(set(fields)) != len(fields)
        ):
            return None
        if not self._native_authoritative and not self._simple_base_eligible():
            return None

        clone = self._chain()
        plan = clone._native_plan_candidate("values", fields)
        if plan is None:
            return None
        clone._native_store_plan(plan)
        clone._fields = tuple(fields)
        handle = clone._native_qs
        if handle is not None:
            try:
                if iterable_class is ValuesIterable:
                    projected = handle.with_values(list(fields))
                else:
                    projected = handle.with_values_list(
                        list(fields), iterable_class is FlatValuesListIterable
                    )
            except Exception:
                projected = None
            if projected is None:
                clone._native_materialize_python_query()
                clone._native_disabled = True
                clone._native_qs = None
                clone.query.set_values(fields)
                clone._iterable_class = iterable_class
                return clone
            clone._native_qs = projected

        clone._iterable_class = iterable_class
        return clone

    def values(self, *fields, **expressions):
        if not expressions and fields:
            native = self._native_simple_values_projection(fields, ValuesIterable)
            if native is not None:
                return native
        fields += tuple(expressions)
        clone = self._values(*fields, **expressions)
        clone._iterable_class = ValuesIterable
        return clone

    def values_list(self, *fields, flat=False, named=False):
        from django import native as _native

        # Keep validation in pure Python (native hop was net-negative on hot paths).
        if flat and named:
            raise TypeError("'flat' and 'named' can't be used together.")
        if flat and len(fields) > 1:
            raise TypeError(
                "'flat' is not valid when values_list is called with more than one "
                "field."
            )
        if flat and not fields:
            fields = [self.model._meta.concrete_fields[0].attname]

        if not named and fields and all(isinstance(field, str) for field in fields):
            iterable_class = FlatValuesListIterable if flat else ValuesListIterable
            native = self._native_simple_values_projection(fields, iterable_class)
            if native is not None:
                return native

        field_names = {f: False for f in fields if not hasattr(f, "resolve_expression")}
        _fields = []
        expressions = {}
        counter = 1
        for field in fields:
            field_name = field
            expression = None
            if hasattr(field, "resolve_expression"):
                field_name = getattr(
                    field, "default_alias", field.__class__.__name__.lower()
                )
                expression = field
                # For backward compatibility reasons expressions are always
                # prefixed with the counter even if their default alias doesn't
                # collide with field names. Changing this logic could break
                # some usage of named=True.
                seen = True
            elif seen := field_names[field_name]:
                expression = F(field_name)
            if seen:
                field_name_prefix = field_name
                if _native.AVAILABLE:
                    field_name = _native.unique_field_alias(
                        field_name_prefix, counter, list(field_names.keys())
                    )
                    # Keep counter ahead of any suffix we used.
                    suffix = field_name[len(field_name_prefix) :]
                    if suffix.isdigit():
                        counter = int(suffix) + 1
                else:
                    while (
                        field_name := f"{field_name_prefix}{counter}"
                    ) in field_names:
                        counter += 1
            if expression is not None:
                expressions[field_name] = expression
            field_names[field_name] = True
            _fields.append(field_name)

        clone = self._values(*_fields, **expressions)
        clone._iterable_class = (
            NamedValuesListIterable
            if named
            else FlatValuesListIterable if flat else ValuesListIterable
        )
        return clone

    def dates(self, field_name, kind, order="ASC"):
        """
        Return a list of date objects representing all available dates for
        the given field_name, scoped to 'kind'.
        """
        from django import native as _native

        if _native.AVAILABLE:
            if not _native.date_kind_valid(kind):
                raise ValueError(
                    "'kind' must be one of 'year', 'month', 'week', or 'day'."
                )
            if not _native.date_order_valid(order):
                raise ValueError("'order' must be either 'ASC' or 'DESC'.")
            order_prefix = _native.order_by_desc_prefix(order)
        else:
            if kind not in ("year", "month", "week", "day"):
                raise ValueError(
                    "'kind' must be one of 'year', 'month', 'week', or 'day'."
                )
            if order not in ("ASC", "DESC"):
                raise ValueError("'order' must be either 'ASC' or 'DESC'.")
            order_prefix = "-" if order == "DESC" else ""
        return (
            self.annotate(
                datefield=Trunc(field_name, kind, output_field=DateField()),
                plain_field=F(field_name),
            )
            .values_list("datefield", flat=True)
            .distinct()
            .filter(plain_field__isnull=False)
            .order_by(order_prefix + "datefield")
        )

    def datetimes(self, field_name, kind, order="ASC", tzinfo=None):
        """
        Return a list of datetime objects representing all available
        datetimes for the given field_name, scoped to 'kind'.
        """
        from django import native as _native

        if _native.AVAILABLE:
            if not _native.datetime_kind_valid(kind):
                raise ValueError(
                    "'kind' must be one of 'year', 'month', 'week', 'day', "
                    "'hour', 'minute', or 'second'."
                )
            if not _native.date_order_valid(order):
                raise ValueError("'order' must be either 'ASC' or 'DESC'.")
            order_prefix = _native.order_by_desc_prefix(order)
        else:
            if kind not in (
                "year",
                "month",
                "week",
                "day",
                "hour",
                "minute",
                "second",
            ):
                raise ValueError(
                    "'kind' must be one of 'year', 'month', 'week', 'day', "
                    "'hour', 'minute', or 'second'."
                )
            if order not in ("ASC", "DESC"):
                raise ValueError("'order' must be either 'ASC' or 'DESC'.")
            order_prefix = "-" if order == "DESC" else ""
        if settings.USE_TZ:
            if tzinfo is None:
                tzinfo = timezone.get_current_timezone()
        else:
            tzinfo = None
        return (
            self.annotate(
                datetimefield=Trunc(
                    field_name,
                    kind,
                    output_field=DateTimeField(),
                    tzinfo=tzinfo,
                ),
                plain_field=F(field_name),
            )
            .values_list("datetimefield", flat=True)
            .distinct()
            .filter(plain_field__isnull=False)
            .order_by(order_prefix + "datetimefield")
        )

    def none(self):
        """Return an empty QuerySet."""
        clone = self._chain()
        clone.query.set_empty()
        return clone

    ##################################################################
    # PUBLIC METHODS THAT ALTER ATTRIBUTES AND RETURN A NEW QUERYSET #
    ##################################################################

    def all(self):
        """
        Return a new QuerySet that is a copy of the current one. This allows a
        QuerySet to proxy for a model manager in some cases.
        """
        return self._chain()

    def filter(self, *args, **kwargs):
        """
        Return a new QuerySet instance with the args ANDed to the existing
        set.
        """
        self._not_support_combined_queries("filter")
        return self._filter_or_exclude(False, args, kwargs)

    def exclude(self, *args, **kwargs):
        """
        Return a new QuerySet instance with NOT (args) ANDed to the existing
        set.
        """
        self._not_support_combined_queries("exclude")
        return self._filter_or_exclude(True, args, kwargs)

    def _filter_or_exclude(self, negate, args, kwargs):
        if (
            (args or kwargs)
            and self._query is not None
            and self._query.is_sliced
        ):
            raise TypeError("Cannot filter a query once a slice has been taken.")
        clone = self._chain()
        if self._defer_next_filter:
            self._defer_next_filter = False
            clone._deferred_filter = negate, args, kwargs
        else:
            invalid_kwargs = PROHIBITED_FILTER_KWARGS.intersection(kwargs)
            if invalid_kwargs:
                invalid_kwargs_str = ", ".join(f"'{k}'" for k in sorted(invalid_kwargs))
                raise TypeError(
                    f"The following kwargs are invalid: {invalid_kwargs_str}"
                )
            if not clone._native_defer_simple_filter(
                negate, args, kwargs
            ) and not clone._native_apply_filter(negate, args, kwargs):
                clone._native_materialize_python_query()
                clone._filter_or_exclude_inplace(negate, args, kwargs)
                clone._native_disabled = True
                clone._native_qs = None
        return clone

    def _filter_or_exclude_inplace(self, negate, args, kwargs):
        if invalid_kwargs := PROHIBITED_FILTER_KWARGS.intersection(kwargs):
            invalid_kwargs_str = ", ".join(f"'{k}'" for k in sorted(invalid_kwargs))
            raise TypeError(f"The following kwargs are invalid: {invalid_kwargs_str}")
        query = self._query
        if query is None:
            query = self._query = sql.Query(self.model)
        if negate:
            query.add_q(~Q(*args, **kwargs))
        else:
            query.add_q(Q(*args, **kwargs))

    def _native_eligible_for_dataplane(self):
        """Query shapes the C++ data plane can still own."""
        query = self._query
        if query is None:
            return True
        # Annotations/select_related may be owned by the native handle itself.
        if (
            query.combinator
            or query.extra
            or query.distinct_fields
            or query.select_for_update
        ):
            return False
        return True

    def _native_store_plan(self, plan):
        if plan is None:
            return False
        self._native_plan = plan
        self._native_authoritative = True
        return True

    def _native_plan_candidate(self, operation, payload):
        """Validate one operation against the registered C++ model schema."""
        from django.native import orm as native_orm

        orm = native_orm._orm()
        if orm is None:
            return None
        plan = self._native_plan
        if plan is not None:
            if operation == "q":
                return plan.with_q(payload)
            if operation == "ordering":
                return plan.with_ordering(payload)
            if operation == "values":
                return plan.with_values(payload)
            return None
        mid = native_orm.register_model_from_meta(self.model)
        if mid is None:
            return None
        if operation == "q":
            return orm.QueryPlan.for_q(int(mid), payload)
        if operation == "ordering":
            return orm.QueryPlan.for_ordering(int(mid), payload)
        if operation == "values":
            return orm.QueryPlan.for_values(int(mid), payload)
        return None

    def _native_plan_with_q(self, tree):
        return self._native_store_plan(self._native_plan_candidate("q", tree))

    def _native_plan_with_ordering(self, field_names):
        return self._native_store_plan(
            self._native_plan_candidate("ordering", field_names)
        )

    def _native_plan_with_values(self, fields):
        return self._native_store_plan(
            self._native_plan_candidate("values", fields)
        )

    def _native_materialize_python_query(self):
        """Replay the compact native plan into a Python Query on demand."""
        if not self._native_authoritative:
            return
        query = (
            self._query.chain()
            if self._query is not None
            else sql.Query(self.model)
        )
        replay = self._native_plan.replay() if self._native_plan is not None else ()
        for operation in replay:
            kind = operation[0]
            if kind == "filter":
                query.add_q(_q_from_native_tree(operation[1]))
            elif kind == "order_by":
                query.clear_ordering(force=True, clear_default=False)
                query.add_ordering(*operation[1])
            elif kind == "values":
                query.set_values(operation[1])
        self._query = query
        self._native_authoritative = False
        self._native_plan = None
        self._native_disabled = True
        self._native_qs = None

    def _native_has_only_projection(self):
        return (
            self._native_authoritative
            and self._native_plan is not None
            and self._native_plan.has_only_projection()
        )

    def _native_plan_has_simple_filter(self):
        return (
            self._native_authoritative
            and self._native_plan is not None
            and self._native_plan.has_simple_filter()
        )

    def _native_ordering_is_safe(self, field_names):
        if self.model._meta.parents:
            return False
        for item in field_names:
            if not isinstance(item, str) or item == "?":
                return False
            name = item.removeprefix("-")
            if not name or LOOKUP_SEP in name:
                return False
        return True

    def _native_defer_simple_filter(self, negate, args, kwargs):
        """Keep one safe primitive exact/IN filter for a fused terminal call."""
        if (
            negate
            or args
            or len(kwargs) != 1
            or self._native_authoritative
            or self._native_qs is not None
            or self._native_disabled
            or self.model._meta.parents
            or self.model._meta.ordering
            or not self._simple_base_eligible()
        ):
            return False
        try:
            from django.native import orm as native_orm

            orm = native_orm._orm()
            if orm is None:
                return False
            key, value = next(iter(kwargs.items()))
            if not isinstance(key, str):
                return False
            parts = key.split(LOOKUP_SEP)
            lookup_in = len(parts) == 2 and parts[1] == "in"
            if len(parts) != (2 if lookup_in else 1):
                return False
            if lookup_in:
                if not isinstance(value, (list, tuple)) or not value:
                    return False
                values = tuple(value)
            else:
                values = (value,)
            mid = native_orm.register_model_from_meta(self.model)
            if mid is None:
                return False
            plan = orm.QueryPlan.for_simple_filter(
                int(mid), parts[0], lookup_in, values
            )
            return self._native_store_plan(plan)
        except Exception:
            return False

    def _native_apply_filter(self, negate, args, kwargs):
        """Apply a filter to the authoritative C++ graph without Python Query."""
        if not args and not kwargs:
            return True
        if getattr(self, "_native_disabled", False):
            return False
        try:
            from django.native import orm as native_orm

            orm = native_orm._orm()
            if orm is None or not self._native_eligible_for_dataplane():
                return False
            if not self._native_authoritative and not self._simple_base_eligible():
                return False
            q_obj = Q(*args, **kwargs)
            if negate:
                q_obj = ~q_obj
            if self.model._meta.ordering:
                return False
            tree = native_orm.q_to_tree(q_obj)
            if tree is None:
                return False
            plan = self._native_plan_candidate("q", tree)
            if plan is None:
                return False
            handle = getattr(self, "_native_qs", None)
            # A deferred simple filter has no C++ handle yet. Starting a new
            # handle from only this Q object would silently omit that first
            # predicate, so let the caller replay both filters into Python.
            if handle is None and self._native_plan_has_simple_filter():
                return False
            if handle is None:
                mid = native_orm.register_model_from_meta(self.model)
                dialect = native_orm.dialect_for_connection(connections[self.db])
                if mid is None or dialect is None:
                    return False
                handle = orm.QuerySet.create_from_native_q(
                    int(mid), int(dialect), tree
                )
                if handle is None:
                    return False
                # values()/values_list() may precede filter().
                if self._fields is not None:
                    if self._iterable_class is ValuesIterable:
                        if not handle.values(list(self._fields)):
                            return False
                    elif self._iterable_class in (
                        ValuesListIterable,
                        FlatValuesListIterable,
                    ):
                        if not handle.values_list(
                            list(self._fields),
                            self._iterable_class is FlatValuesListIterable,
                        ):
                            return False
                self._native_qs = handle
            else:
                handle = handle.with_native_q(tree)
                if handle is None:
                    return False
                self._native_qs = handle
            return self._native_store_plan(plan)
        except Exception:
            return False

    def _native_handle_for_sql(self):
        if getattr(self, "_native_disabled", False) or not self._native_authoritative:
            return None
        handle = getattr(self, "_native_qs", None)
        if handle is None:
            return None
        if not self._native_eligible_for_dataplane():
            return None
        return handle

    def complex_filter(self, filter_obj):
        """
        Return a new QuerySet instance with filter_obj added to the filters.

        filter_obj can be a Q object or a dictionary of keyword lookup
        arguments.

        This exists to support framework features such as 'limit_choices_to',
        and usually it will be more natural to use other methods.
        """
        from django import native as _native

        is_q = isinstance(filter_obj, Q)
        if _native.AVAILABLE:
            if _native.complex_filter_is_q(is_q):
                clone = self._chain()
                clone.query.add_q(filter_obj)
                return clone
            return self._filter_or_exclude(False, args=(), kwargs=filter_obj)
        if is_q:
            clone = self._chain()
            clone.query.add_q(filter_obj)
            return clone
        return self._filter_or_exclude(False, args=(), kwargs=filter_obj)

    def _combinator_query(self, combinator, *other_qs, all=False):
        # Clone the query to inherit the select list and everything
        clone = self._chain()
        # Clear limits and ordering so they can be reapplied
        clone.query.clear_ordering(force=True)
        clone.query.clear_limits()
        clone.query.combined_queries = (self.query, *(qs.query for qs in other_qs))
        clone.query.combinator = combinator
        clone.query.combinator_all = all
        return clone

    def union(self, *other_qs, all=False):
        from django import native as _native

        # If the query is an EmptyQuerySet, combine all nonempty querysets.
        if isinstance(self, EmptyQuerySet):
            qs = [q for q in other_qs if not isinstance(q, EmptyQuerySet)]
            if _native.AVAILABLE:
                kind = _native.union_empty_self_kind(len(qs))
                if kind == 0:
                    return self
                if kind == 1:
                    return qs[0]
                return qs[0]._combinator_query("union", *qs[1:], all=all)
            if not qs:
                return self
            if len(qs) == 1:
                return qs[0]
            return qs[0]._combinator_query("union", *qs[1:], all=all)
        elif not other_qs:
            return self
        return self._combinator_query("union", *other_qs, all=all)

    def intersection(self, *other_qs):
        from django import native as _native

        # If any query is an EmptyQuerySet, return it.
        if _native.AVAILABLE:
            if _native.combinator_return_empty_self(isinstance(self, EmptyQuerySet)):
                return self
        elif isinstance(self, EmptyQuerySet):
            return self
        for other in other_qs:
            if isinstance(other, EmptyQuerySet):
                return other
        return self._combinator_query("intersection", *other_qs)

    def difference(self, *other_qs):
        from django import native as _native

        # If the query is an EmptyQuerySet, return it.
        if _native.AVAILABLE:
            if _native.combinator_return_empty_self(isinstance(self, EmptyQuerySet)):
                return self
        elif isinstance(self, EmptyQuerySet):
            return self
        return self._combinator_query("difference", *other_qs)

    def select_for_update(self, nowait=False, skip_locked=False, of=(), no_key=False):
        """
        Return a new QuerySet instance that will select objects with a
        FOR UPDATE lock.
        """
        from django import native as _native

        if _native.AVAILABLE:
            if _native.select_for_update_options_conflict(nowait, skip_locked):
                raise ValueError("The nowait option cannot be used with skip_locked.")
        elif nowait and skip_locked:
            raise ValueError("The nowait option cannot be used with skip_locked.")
        obj = self._chain()
        obj._for_write = True
        obj.query.select_for_update = True
        obj.query.select_for_update_nowait = nowait
        obj.query.select_for_update_skip_locked = skip_locked
        obj.query.select_for_update_of = of
        obj.query.select_for_no_key_update = no_key
        return obj

    def select_related(self, *fields):
        """
        Return a new QuerySet instance that will select related objects.

        If fields are specified, they must be ForeignKey fields and only those
        related objects are included in the selection.

        If select_related(None) is called, clear the list.
        """
        from django import native as _native

        self._not_support_combined_queries("select_related")
        if _native.AVAILABLE:
            code = _native.queryset_write_guard(
                False, False, False, self._fields is not None
            )
            if code == 4:
                raise TypeError(
                    "Cannot call select_related() after .values() or .values_list()"
                )
        elif self._fields is not None:
            raise TypeError(
                "Cannot call select_related() after .values() or .values_list()"
            )

        obj = self._chain()
        if fields == (None,):
            obj.query.select_related = False
        elif fields:
            obj.query.add_select_related(fields)
        else:
            obj.query.select_related = True
        return obj

    def _native_ensure_handle(self):
        """Return a writable native handle or None."""
        if getattr(self, "_native_disabled", False):
            return None
        try:
            from django.native import orm as native_orm

            if native_orm._orm() is None:
                return None
            handle = getattr(self, "_native_qs", None)
            if handle is None:
                if not self._native_eligible_for_dataplane():
                    return None
                handle = native_orm.build_queryset(self.model, connections[self.db])
                if handle is None:
                    return None
                self._native_qs = handle
            else:
                self._native_qs = handle.clone()
            return self._native_qs
        except Exception:
            return None

    def _native_track_select_related(self, fields):
        h = self._native_ensure_handle()
        if h is None:
            return
        try:
            if not h.base_attnames():
                h.select_model_columns()
            for path in fields:
                if not isinstance(path, str) or not h.add_select_related(path):
                    self._native_disabled = True
                    self._native_qs = None
                    return
        except Exception:
            self._native_disabled = True
            self._native_qs = None

    def _native_track_select_related_all(self, max_depth=5):
        h = self._native_ensure_handle()
        if h is None:
            self._native_disabled = True
            self._native_qs = None
            return
        try:
            if not h.base_attnames():
                h.select_model_columns()
            if not h.add_select_related_all(int(max_depth)):
                self._native_disabled = True
                self._native_qs = None
        except Exception:
            self._native_disabled = True
            self._native_qs = None

    def _native_track_annotate(self, annotations, select=True):
        """Mirror Aggregate / Subquery / Case / F / Value / Combined onto native."""
        if getattr(self, "_native_disabled", False) or not select:
            return
        try:
            from django.native import orm as native_orm
            from django.db.models.aggregates import Aggregate
            from django.db.models.expressions import (
                Case,
                CombinedExpression,
                F,
                Star,
                Subquery,
                Value,
                When,
            )

            if native_orm._orm() is None:
                return
            h = self._native_ensure_handle()
            if h is None:
                return
            if not h.base_attnames():
                h.select_model_columns()
            for alias, expr in annotations.items():
                if isinstance(expr, Aggregate):
                    func = expr.function
                    star = False
                    field_name = ""
                    distinct = bool(getattr(expr, "distinct", False))
                    sources = expr.get_source_expressions()
                    if not sources or isinstance(sources[0], Star):
                        star = True
                    else:
                        src = sources[0]
                        name = getattr(src, "name", None)
                        if isinstance(name, str):
                            field_name = name
                        else:
                            self._native_disabled = True
                            self._native_qs = None
                            return
                    if not h.annotate_aggregate(
                        alias, func, field_name, distinct, star
                    ):
                        self._native_disabled = True
                        self._native_qs = None
                        return
                    h.group_by_selected_columns()
                elif isinstance(expr, Subquery):
                    # Prefer fully native nested QuerySet when possible.
                    sub_qs = getattr(expr, "queryset", None)
                    if (
                        sub_qs is not None
                        and getattr(sub_qs, "_native_qs", None) is not None
                    ):
                        if not h.annotate_subquery_qs(alias, sub_qs._native_qs):
                            self._native_disabled = True
                            self._native_qs = None
                            return
                    else:
                        # Build native sub from model+empty and try simple values
                        try:
                            inner = expr.query
                            # Compile via a throwaway native QS if simple
                            sub_handle = native_orm.build_queryset(
                                inner.model, connections[self.db]
                            )
                            if sub_handle is None:
                                raise ValueError("no sub handle")
                            # Best-effort: only empty-where values subqueries
                            if not h.annotate_subquery_qs(alias, sub_handle):
                                raise ValueError("annotate sub failed")
                        except Exception:
                            self._native_disabled = True
                            self._native_qs = None
                            return
                elif isinstance(expr, Case):
                    cases = []
                    for w in expr.cases:
                        if not isinstance(w, When):
                            self._native_disabled = True
                            self._native_qs = None
                            return
                        cond = w.condition
                        # When.condition is often a Q or lookup
                        from django.db.models import Q as Qobj

                        if not isinstance(cond, Qobj):
                            # Try wrap single node
                            try:
                                cond = Qobj(cond) if False else None
                            except Exception:
                                cond = None
                        if cond is None:
                            # condition may be a dict-like ResolvedOuter?
                            # Fall back: use Q from children if present
                            self._native_disabled = True
                            self._native_qs = None
                            return
                        tree = native_orm.q_to_tree(cond)
                        if tree is None:
                            self._native_disabled = True
                            self._native_qs = None
                            return
                        then = w.result
                        if isinstance(then, Value):
                            then_v = then.value
                        elif isinstance(then, (str, int, float, bool, type(None))):
                            then_v = then
                        else:
                            self._native_disabled = True
                            self._native_qs = None
                            return
                        cases.append((tree, then_v))
                    else_v = expr.default
                    has_else = else_v is not None
                    if has_else and isinstance(else_v, Value):
                        else_v = else_v.value
                    elif has_else and not isinstance(
                        else_v, (str, int, float, bool, type(None))
                    ):
                        self._native_disabled = True
                        self._native_qs = None
                        return
                    if not h.annotate_case(alias, cases, has_else, else_v):
                        self._native_disabled = True
                        self._native_qs = None
                        return
                elif isinstance(expr, Value):
                    if not h.annotate_value(alias, expr.value):
                        self._native_disabled = True
                        self._native_qs = None
                        return
                elif isinstance(expr, F):
                    if not h.annotate_f(alias, expr.name):
                        self._native_disabled = True
                        self._native_qs = None
                        return
                elif isinstance(expr, CombinedExpression):
                    lhs, rhs = expr.lhs, expr.rhs
                    connector = expr.connector  # '+', '-', etc.
                    if isinstance(lhs, F) and isinstance(rhs, Value):
                        ok = h.annotate_binop(alias, lhs.name, connector, rhs.value)
                    elif isinstance(lhs, F) and isinstance(rhs, F):
                        ok = h.annotate_binop_fields(
                            alias, lhs.name, connector, rhs.name
                        )
                    elif isinstance(lhs, Value) and isinstance(rhs, F):
                        # rewrite as field op is awkward; poison
                        ok = False
                    else:
                        ok = False
                    if not ok:
                        self._native_disabled = True
                        self._native_qs = None
                        return
                else:
                    self._native_disabled = True
                    self._native_qs = None
                    return
        except Exception:
            self._native_disabled = True
            self._native_qs = None

    def prefetch_related(self, *lookups):
        """
        Return a new QuerySet instance that will prefetch the specified
        Many-To-One and Many-To-Many related objects when the QuerySet is
        evaluated.

        When prefetch_related() is called more than once, append to the list of
        prefetch lookups. If prefetch_related(None) is called, clear the list.
        """
        self._not_support_combined_queries("prefetch_related")
        clone = self._chain()
        if lookups == (None,):
            clone._prefetch_related_lookups = ()
        else:
            for lookup in lookups:
                if isinstance(lookup, Prefetch):
                    lookup = lookup.prefetch_to
                lookup = lookup.split(LOOKUP_SEP, 1)[0]
                if lookup in self.query._filtered_relations:
                    raise ValueError(
                        "prefetch_related() is not supported with FilteredRelation."
                    )
            clone._prefetch_related_lookups = clone._prefetch_related_lookups + lookups
        return clone

    def annotate(self, *args, **kwargs):
        """
        Return a query set in which the returned objects have been annotated
        with extra data or aggregations.
        """
        self._not_support_combined_queries("annotate")
        return self._annotate(args, kwargs, select=True)

    def alias(self, *args, **kwargs):
        """
        Return a query set with added aliases for extra data or aggregations.
        """
        self._not_support_combined_queries("alias")
        return self._annotate(args, kwargs, select=False)

    def _annotate(self, args, kwargs, select=True):
        from django import native as _native

        self._validate_values_are_expressions(
            args + tuple(kwargs.values()), method_name="annotate"
        )
        annotations = {}
        for arg in args:
            # The default_alias property raises TypeError if default_alias
            # can't be set automatically or AttributeError if it isn't an
            # attribute.
            try:
                if arg.default_alias in kwargs:
                    raise ValueError(
                        "The named annotation '%s' conflicts with the "
                        "default name for another annotation." % arg.default_alias
                    )
            except (TypeError, AttributeError):
                raise TypeError("Complex annotations require an alias")
            annotations[arg.default_alias] = arg
        annotations.update(kwargs)

        clone = self._chain()
        names = self._fields
        if names is None:
            names = set(
                chain.from_iterable(
                    (
                        (field.name, field.attname)
                        if hasattr(field, "attname")
                        else (field.name,)
                    )
                    for field in self.model._meta.get_fields()
                )
            )

        for alias, annotation in annotations.items():
            if _native.AVAILABLE:
                if _native.annotation_alias_conflicts(alias in names):
                    raise ValueError(
                        "The annotation '%s' conflicts with a field on "
                        "the model." % alias
                    )
            elif alias in names:
                raise ValueError(
                    "The annotation '%s' conflicts with a field on "
                    "the model." % alias
                )
            if isinstance(annotation, FilteredRelation):
                clone.query.add_filtered_relation(annotation, alias)
            else:
                clone.query.add_annotation(
                    annotation,
                    alias,
                    select=select,
                )
        for alias, annotation in clone.query.annotations.items():
            if alias in annotations and annotation.contains_aggregate:
                if clone._fields is None:
                    clone.query.group_by = True
                else:
                    clone.query.set_group_by()
                break

        return clone

    def order_by(self, *field_names):
        """Return a new QuerySet instance with the ordering changed."""
        from django import native as _native

        if self._query is not None and self._query.is_sliced:
            raise TypeError("Cannot reorder a query once a slice has been taken.")
        obj = self._chain()
        if _native.AVAILABLE and obj._native_ordering_is_safe(field_names):
            if (
                obj._native_qs is None
                and obj._native_authoritative
                and obj._native_plan_has_simple_filter()
            ):
                if obj._native_plan_with_ordering(field_names):
                    return obj
            try:
                from django.native import orm as native_orm

                handle = obj._native_qs
                if handle is not None and not obj._native_authoritative:
                    handle = None
                if handle is None and obj._simple_base_eligible():
                    handle = native_orm.build_queryset(obj.model, connections[obj.db])
                    if handle is not None and obj._fields is not None:
                        if obj._iterable_class is ValuesIterable:
                            projected = handle.values(list(obj._fields))
                        else:
                            projected = handle.values_list(
                                list(obj._fields),
                                obj._iterable_class is FlatValuesListIterable,
                            )
                        if not projected:
                            handle = None
                ordered = (
                    handle.with_ordering(list(field_names))
                    if handle is not None
                    else None
                )
                if ordered is not None:
                    obj._native_qs = ordered
                    if obj._native_plan_with_ordering(field_names):
                        return obj
            except Exception:
                pass

        obj._native_materialize_python_query()
        obj._native_disabled = True
        obj._native_qs = None
        obj.query.clear_ordering(force=True, clear_default=False)
        obj.query.add_ordering(*field_names)
        return obj

    def distinct(self, *field_names):
        """
        Return a new QuerySet instance that will select only distinct results.
        """
        from django import native as _native

        self._not_support_combined_queries("distinct")
        if _native.AVAILABLE:
            if _native.queryset_sliced_error(self.query.is_sliced):
                raise TypeError(
                    "Cannot create distinct fields once a slice has been taken."
                )
        elif self.query.is_sliced:
            raise TypeError(
                "Cannot create distinct fields once a slice has been taken."
            )
        obj = self._chain()
        obj.query.add_distinct_fields(*field_names)
        return obj

    def extra(
        self,
        select=None,
        where=None,
        params=None,
        tables=None,
        order_by=None,
        select_params=None,
    ):
        """Add extra SQL fragments to the query."""
        from django import native as _native

        self._not_support_combined_queries("extra")
        if _native.AVAILABLE:
            if _native.queryset_sliced_error(self.query.is_sliced):
                raise TypeError("Cannot change a query once a slice has been taken.")
        elif self.query.is_sliced:
            raise TypeError("Cannot change a query once a slice has been taken.")
        clone = self._chain()
        clone.query.add_extra(select, select_params, where, params, tables, order_by)
        return clone

    def reverse(self):
        """Reverse the ordering of the QuerySet."""
        from django import native as _native

        if _native.AVAILABLE:
            if _native.queryset_sliced_error(self.query.is_sliced):
                raise TypeError("Cannot reverse a query once a slice has been taken.")
        elif self.query.is_sliced:
            raise TypeError("Cannot reverse a query once a slice has been taken.")
        clone = self._chain()
        if _native.AVAILABLE:
            clone.query.standard_ordering = _native.reverse_standard_ordering(
                clone.query.standard_ordering
            )
        else:
            clone.query.standard_ordering = not clone.query.standard_ordering
        return clone

    def defer(self, *fields):
        """
        Defer the loading of data for certain fields until they are accessed.
        Add the set of deferred fields to any existing set of deferred fields.
        The only exception to this is if None is passed in as the only
        parameter, in which case remove all deferrals.
        """
        from django import native as _native

        self._not_support_combined_queries("defer")
        if _native.AVAILABLE:
            code = _native.queryset_write_guard(
                False, False, False, self._fields is not None
            )
            if code == 4:
                raise TypeError("Cannot call defer() after .values() or .values_list()")
        elif self._fields is not None:
            raise TypeError("Cannot call defer() after .values() or .values_list()")
        clone = self._chain()
        clear = (
            _native.clear_none_arg(fields == (None,))
            if _native.AVAILABLE
            else fields == (None,)
        )
        if clear:
            clone.query.clear_deferred_loading()
        else:
            clone.query.add_deferred_loading(fields)
        return clone

    def only(self, *fields):
        """
        Essentially, the opposite of defer(). Only the fields passed into this
        method and that are not already specified as deferred are loaded
        immediately when the queryset is evaluated.
        """
        from django import native as _native

        self._not_support_combined_queries("only")
        if _native.AVAILABLE:
            code = _native.queryset_write_guard(
                False, False, False, self._fields is not None
            )
            if code == 4:
                raise TypeError("Cannot call only() after .values() or .values_list()")
            if _native.only_none_arg_error(fields == (None,)):
                raise TypeError("Cannot pass None as an argument to only().")
        else:
            if self._fields is not None:
                raise TypeError("Cannot call only() after .values() or .values_list()")
            if fields == (None,):
                # Can only pass None to defer(), not only(), as the rest option.
                # That won't stop people trying to do this, so let's be explicit.
                raise TypeError("Cannot pass None as an argument to only().")
        for field in fields:
            if _native.AVAILABLE:
                field = _native.lookup_head(str(field))
            else:
                field = field.split(LOOKUP_SEP, 1)[0]
            if field in self.query._filtered_relations:
                raise ValueError("only() is not supported with FilteredRelation.")
        clone = self._chain()
        clone.query.add_immediate_loading(fields)
        return clone

    def using(self, alias):
        """Select which database this QuerySet should execute against."""
        clone = self._chain()
        clone._db = alias
        return clone

    ###################################
    # PUBLIC INTROSPECTION ATTRIBUTES #
    ###################################

    @property
    def ordered(self):
        """
        Return True if the QuerySet is ordered -- i.e. has an order_by()
        clause or a default ordering on the model (or is empty).
        """
        from django import native as _native

        if _native.AVAILABLE:
            return _native.queryset_is_ordered(
                isinstance(self, EmptyQuerySet),
                bool(self.query.extra_order_by),
                bool(self.query.order_by),
                bool(self.query.default_ordering),
                bool(self.query.get_meta().ordering),
                bool(self.query.group_by),
            )
        if isinstance(self, EmptyQuerySet):
            return True
        if self.query.extra_order_by or self.query.order_by:
            return True
        elif (
            self.query.default_ordering
            and self.query.get_meta().ordering
            and
            # A default ordering doesn't affect GROUP BY queries.
            not self.query.group_by
        ):
            return True
        else:
            return False

    @property
    def db(self):
        """Return the database used if this query is executed now."""
        if self._for_write:
            return self._db or router.db_for_write(self.model, **self._hints)
        return self._db or router.db_for_read(self.model, **self._hints)

    ###################
    # PRIVATE METHODS #
    ###################

    def _insert(
        self,
        objs,
        fields,
        returning_fields=None,
        raw=False,
        using=None,
        on_conflict=None,
        update_fields=None,
        unique_fields=None,
    ):
        """
        Insert a new record for the given model. This provides an interface to
        the InsertQuery class and is how Model.save() is implemented.
        """
        self._for_write = True
        if using is None:
            using = self.db
        query = sql.InsertQuery(
            self.model,
            on_conflict=on_conflict,
            update_fields=update_fields,
            unique_fields=unique_fields,
        )
        query.insert_values(fields, objs, raw=raw)
        return query.get_compiler(using=using).execute_sql(returning_fields)

    _insert.alters_data = True
    _insert.queryset_only = False

    def _batched_insert(
        self,
        objs,
        fields,
        batch_size,
        on_conflict=None,
        update_fields=None,
        unique_fields=None,
    ):
        """
        Helper method for bulk_create() to insert objs one batch at a time.
        """
        from django import native as _native

        connection = connections[self.db]
        ops = connection.ops
        max_batch_size = max(ops.bulk_batch_size(fields, objs), 1)
        if _native.AVAILABLE:
            batch_size = _native.effective_batch_size(
                batch_size is not None,
                0 if batch_size is None else int(batch_size),
                max_batch_size,
                len(objs),
            )
            batches = [
                objs[start:end]
                for start, end in _native.in_bulk_batch_ranges(len(objs), batch_size)
            ]
        else:
            batch_size = (
                min(batch_size, max_batch_size) if batch_size else max_batch_size
            )
            batches = [
                objs[i : i + batch_size] for i in range(0, len(objs), batch_size)
            ]
        inserted_rows = []
        returning_fields = (
            self.model._meta.db_returning_fields
            if (
                connection.features.can_return_rows_from_bulk_insert
                and (on_conflict is None or on_conflict == OnConflict.UPDATE)
            )
            else None
        )
        if _native.AVAILABLE:
            needs_atomic = _native.multi_batch_needs_atomic(len(batches))
        else:
            needs_atomic = len(batches) > 1
        if needs_atomic:
            context = transaction.atomic(using=self.db, savepoint=False)
        else:
            context = nullcontext()
        with context:
            for item in batches:
                inserted_rows.extend(
                    self._insert(
                        item,
                        fields=fields,
                        using=self.db,
                        on_conflict=on_conflict,
                        update_fields=update_fields,
                        unique_fields=unique_fields,
                        returning_fields=returning_fields,
                    )
                )
        return inserted_rows

    def _chain(self):
        """
        Return a copy of the current QuerySet that's ready for another
        operation.
        """
        obj = self._clone()
        if obj._sticky_filter:
            obj.query.filter_is_sticky = True
            obj._sticky_filter = False
        return obj

    def _clone(self):
        """
        Return a copy of the current QuerySet. A lightweight alternative
        to deepcopy().
        """
        native_authoritative = getattr(self, "_native_authoritative", False)
        if self._deferred_filter:
            # Related managers deliberately defer their core filter until the
            # Query is cloned. Preserve Django's property-access semantics.
            query = self.query.chain()
            native_authoritative = False
        else:
            query = (
                self._query
                if native_authoritative or self._query is None
                else self.query.chain()
            )
        c = self.__class__(
            model=self.model, query=query, using=self._db, hints=self._hints
        )
        c._sticky_filter = self._sticky_filter
        c._for_write = self._for_write
        c._prefetch_related_lookups = self._prefetch_related_lookups[:]
        c._known_related_objects = self._known_related_objects
        c._iterable_class = self._iterable_class
        c._fields = self._fields
        # Native mutations are functional COW operations, so cloning the
        # Python QuerySet can share this wrapper without a native crossing.
        parent_native = getattr(self, "_native_qs", None)
        if parent_native is not None:
            c._native_qs = parent_native
            c._native_disabled = getattr(self, "_native_disabled", False)
        else:
            c._native_qs = None
            c._native_disabled = getattr(self, "_native_disabled", False)
        c._native_authoritative = native_authoritative
        c._native_plan = getattr(self, "_native_plan", None)
        return c

    def _fetch_all(self):
        # Pure Python control flow (native bool hops were net-negative on TE).
        # Prefer C++ data-plane terminals when a live handle exists.
        if self._result_cache is None:
            fast = self._fast_path_simple_values_fetch()
            if fast is not _FAST_PATH_MISS:
                self._result_cache = fast
            else:
                models = self._native_terminal_fetch_all()
                if models is not _FAST_PATH_MISS:
                    self._result_cache = models
                else:
                    self._result_cache = list(self._iterable_class(self))
        if self._prefetch_related_lookups and not self._prefetch_done:
            self._prefetch_related_objects()

    def _next_is_sticky(self):
        """
        Indicate that the next filter call and the one following that should
        be treated as a single filter. This is only important when it comes to
        determining when to reuse tables for many-to-many filters. Required so
        that we can filter naturally on the results of related managers.

        This doesn't return a clone of the current QuerySet (it returns
        "self"). The method is only used internally and should be immediately
        followed by a filter() that does create a clone.
        """
        self._sticky_filter = True
        return self

    def _merge_sanity_check(self, other):
        """Check that two QuerySet classes may be merged."""
        if self._fields is not None and (
            set(self.query.values_select) != set(other.query.values_select)
            or set(self.query.extra_select) != set(other.query.extra_select)
            or set(self.query.annotation_select) != set(other.query.annotation_select)
        ):
            raise TypeError(
                "Merging '%s' classes must involve the same values in each case."
                % self.__class__.__name__
            )

    def _merge_known_related_objects(self, other):
        """
        Keep track of all known related objects from either QuerySet instance.
        """
        for field, objects in other._known_related_objects.items():
            self._known_related_objects.setdefault(field, {}).update(objects)

    def resolve_expression(self, *args, **kwargs):
        query = self.query.resolve_expression(*args, **kwargs)
        query._db = self._db
        return query

    resolve_expression.queryset_only = True

    def _add_hints(self, **hints):
        """
        Update hinting information for use by routers. Add new key/values or
        overwrite existing key/values.
        """
        self._hints.update(hints)

    def _has_filters(self):
        """
        Check if this QuerySet has any filtering going on. This isn't
        equivalent with checking if all objects are present in results, for
        example, qs[1:]._has_filters() -> False.
        """
        return self.query.has_filters()

    @staticmethod
    def _validate_values_are_expressions(values, method_name):
        invalid_args = sorted(
            str(arg) for arg in values if not hasattr(arg, "resolve_expression")
        )
        if invalid_args:
            raise TypeError(
                "QuerySet.%s() received non-expression(s): %s."
                % (
                    method_name,
                    ", ".join(invalid_args),
                )
            )

    def _not_support_combined_queries(self, operation_name):
        combinator = self._query.combinator if self._query is not None else None
        if combinator:
            raise NotSupportedError(
                "Calling QuerySet.%s() after %s() is not supported."
                % (operation_name, combinator)
            )

    def _check_operator_queryset(self, other, operator_):
        if self.query.combinator or other.query.combinator:
            raise TypeError(f"Cannot use {operator_} operator with combined queryset.")

    def _check_ordering_first_last_queryset_aggregation(self, method):
        if (
            isinstance(self.query.group_by, tuple)
            # Raise if the pk fields are not in the group_by.
            and self.model._meta.pk
            not in {col.output_field for col in self.query.group_by}
            and set(self.model._meta.pk_fields).difference(
                {col.target for col in self.query.group_by}
            )
        ):
            raise TypeError(
                f"Cannot use QuerySet.{method}() on an unordered queryset performing "
                f"aggregation. Add an ordering with order_by()."
            )


class InstanceCheckMeta(type):
    def __instancecheck__(self, instance):
        return isinstance(instance, QuerySet) and instance.query.is_empty()


class EmptyQuerySet(metaclass=InstanceCheckMeta):
    """
    Marker class to checking if a queryset is empty by .none():
        isinstance(qs.none(), EmptyQuerySet) -> True
    """

    def __init__(self, *args, **kwargs):
        raise TypeError("EmptyQuerySet can't be instantiated")


class RawQuerySet:
    """
    Provide an iterator which converts the results of raw SQL queries into
    annotated model instances.
    """

    def __init__(
        self,
        raw_query,
        model=None,
        query=None,
        params=(),
        translations=None,
        using=None,
        hints=None,
    ):
        self.raw_query = raw_query
        self.model = model
        self._db = using
        self._hints = hints or {}
        self.query = query or sql.RawQuery(sql=raw_query, using=self.db, params=params)
        self.params = params
        self.translations = translations or {}
        self._result_cache = None
        self._prefetch_related_lookups = ()
        self._prefetch_done = False

    def resolve_model_init_order(self):
        """Resolve the init field names and value positions."""
        converter = connections[self.db].introspection.identifier_converter
        model_init_fields = [
            field
            for column_name, field in self.model_fields.items()
            if column_name in self.columns
        ]
        annotation_fields = [
            (column, pos)
            for pos, column in enumerate(self.columns)
            if column not in self.model_fields
        ]
        model_init_order = [
            self.columns.index(converter(f.column)) for f in model_init_fields
        ]
        model_init_names = [f.attname for f in model_init_fields]
        return model_init_names, model_init_order, annotation_fields

    def prefetch_related(self, *lookups):
        """Same as QuerySet.prefetch_related()"""
        clone = self._clone()
        if lookups == (None,):
            clone._prefetch_related_lookups = ()
        else:
            clone._prefetch_related_lookups = clone._prefetch_related_lookups + lookups
        return clone

    def _prefetch_related_objects(self):
        prefetch_related_objects(self._result_cache, *self._prefetch_related_lookups)
        self._prefetch_done = True

    def _clone(self):
        """Same as QuerySet._clone()"""
        c = self.__class__(
            self.raw_query,
            model=self.model,
            query=self.query,
            params=self.params,
            translations=self.translations,
            using=self._db,
            hints=self._hints,
        )
        c._prefetch_related_lookups = self._prefetch_related_lookups[:]
        return c

    def _fetch_all(self):
        if self._result_cache is None:
            self._result_cache = list(self.iterator())
        if self._prefetch_related_lookups and not self._prefetch_done:
            self._prefetch_related_objects()

    def __len__(self):
        self._fetch_all()
        return len(self._result_cache)

    def __bool__(self):
        self._fetch_all()
        return bool(self._result_cache)

    def __iter__(self):
        self._fetch_all()
        return iter(self._result_cache)

    def __aiter__(self):
        # Remember, __aiter__ itself is synchronous, it's the thing it returns
        # that is async!
        async def generator():
            await sync_to_async(self._fetch_all)()
            for item in self._result_cache:
                yield item

        return generator()

    def iterator(self):
        yield from RawModelIterable(self)

    def __repr__(self):
        return "<%s: %s>" % (self.__class__.__name__, self.query)

    def __getitem__(self, k):
        return list(self)[k]

    @property
    def db(self):
        """Return the database used if this query is executed now."""
        return self._db or router.db_for_read(self.model, **self._hints)

    def using(self, alias):
        """Select the database this RawQuerySet should execute against."""
        return RawQuerySet(
            self.raw_query,
            model=self.model,
            query=self.query.chain(using=alias),
            params=self.params,
            translations=self.translations,
            using=alias,
        )

    @cached_property
    def columns(self):
        """
        A list of model field names in the order they'll appear in the
        query results.
        """
        columns = self.query.get_columns()
        # Adjust any column names which don't match field names
        for query_name, model_name in self.translations.items():
            # Ignore translations for nonexistent column names
            try:
                index = columns.index(query_name)
            except ValueError:
                pass
            else:
                columns[index] = model_name
        return columns

    @cached_property
    def model_fields(self):
        """A dict mapping column names to model field names."""
        converter = connections[self.db].introspection.identifier_converter
        return {
            converter(field.column): field
            for field in self.model._meta.fields
            # Fields with None "column" should be ignored
            # (e.g. CompositePrimaryKey).
            if field.column
        }


class Prefetch:
    def __init__(self, lookup, queryset=None, to_attr=None):
        # `prefetch_through` is the path we traverse to perform the prefetch.
        self.prefetch_through = lookup
        # `prefetch_to` is the path to the attribute that stores the result.
        self.prefetch_to = lookup
        if queryset is not None and (
            isinstance(queryset, RawQuerySet)
            or (
                hasattr(queryset, "_iterable_class")
                and not issubclass(queryset._iterable_class, ModelIterable)
            )
        ):
            raise ValueError(
                "Prefetch querysets cannot use raw(), values(), and values_list()."
            )
        if to_attr:
            self.prefetch_to = LOOKUP_SEP.join(
                lookup.split(LOOKUP_SEP)[:-1] + [to_attr]
            )

        self.queryset = queryset
        self.to_attr = to_attr

    def __getstate__(self):
        obj_dict = self.__dict__.copy()
        if self.queryset is not None:
            queryset = self.queryset._chain()
            # Prevent the QuerySet from being evaluated
            queryset._result_cache = []
            queryset._prefetch_done = True
            obj_dict["queryset"] = queryset
        return obj_dict

    def add_prefix(self, prefix):
        self.prefetch_through = prefix + LOOKUP_SEP + self.prefetch_through
        self.prefetch_to = prefix + LOOKUP_SEP + self.prefetch_to

    def get_current_prefetch_to(self, level):
        return LOOKUP_SEP.join(self.prefetch_to.split(LOOKUP_SEP)[: level + 1])

    def get_current_to_attr(self, level):
        parts = self.prefetch_to.split(LOOKUP_SEP)
        to_attr = parts[level]
        as_attr = self.to_attr and level == len(parts) - 1
        return to_attr, as_attr

    def get_current_querysets(self, level):
        if (
            self.get_current_prefetch_to(level) == self.prefetch_to
            and self.queryset is not None
        ):
            return [self.queryset]
        return None

    def __eq__(self, other):
        if not isinstance(other, Prefetch):
            return NotImplemented
        return self.prefetch_to == other.prefetch_to

    def __hash__(self):
        return hash((self.__class__, self.prefetch_to))


def normalize_prefetch_lookups(lookups, prefix=None):
    """Normalize lookups into Prefetch objects."""
    ret = []
    for lookup in lookups:
        if not isinstance(lookup, Prefetch):
            lookup = Prefetch(lookup)
        if prefix:
            lookup.add_prefix(prefix)
        ret.append(lookup)
    return ret


def prefetch_related_objects(model_instances, *related_lookups):
    """
    Populate prefetched object caches for an iterable of model instances based
    on the lookups/Prefetch instances given.
    """
    if not model_instances:
        return  # nothing to do

    # We need to be able to dynamically add to the list of prefetch_related
    # lookups that we look up (see below). So we need some book keeping to
    # ensure we don't do duplicate work.
    done_queries = {}  # dictionary of things like 'foo__bar': [results]

    auto_lookups = set()  # we add to this as we go through.
    followed_descriptors = set()  # recursion protection

    all_lookups = normalize_prefetch_lookups(reversed(related_lookups))
    while all_lookups:
        lookup = all_lookups.pop()
        if lookup.prefetch_to in done_queries:
            if lookup.queryset is not None:
                raise ValueError(
                    "'%s' lookup was already seen with a different queryset. "
                    "You may need to adjust the ordering of your lookups."
                    % lookup.prefetch_to
                )

            continue

        # Top level, the list of objects to decorate is the result cache
        # from the primary QuerySet. It won't be for deeper levels.
        obj_list = model_instances

        through_attrs = lookup.prefetch_through.split(LOOKUP_SEP)
        for level, through_attr in enumerate(through_attrs):
            # Prepare main instances
            if not obj_list:
                break

            prefetch_to = lookup.get_current_prefetch_to(level)
            if prefetch_to in done_queries:
                # Skip any prefetching, and any object preparation
                obj_list = done_queries[prefetch_to]
                continue

            # Prepare objects:
            good_objects = True
            for obj in obj_list:
                # Since prefetching can re-use instances, it is possible to
                # have the same instance multiple times in obj_list, so obj
                # might already be prepared.
                if not hasattr(obj, "_prefetched_objects_cache"):
                    try:
                        obj._prefetched_objects_cache = {}
                    except (AttributeError, TypeError):
                        # Must be an immutable object from
                        # values_list(flat=True), for example (TypeError) or
                        # a QuerySet subclass that isn't returning Model
                        # instances (AttributeError), either in Django or a 3rd
                        # party. prefetch_related() doesn't make sense, so
                        # quit.
                        good_objects = False
                        break
            if not good_objects:
                break

            # Descend down tree

            # We assume that objects retrieved are homogeneous (which is the
            # premise of prefetch_related), so what applies to first object
            # applies to all.
            first_obj = next(iter(obj_list))
            to_attr = lookup.get_current_to_attr(level)[0]
            prefetcher, descriptor, attr_found, is_fetched = get_prefetcher(
                first_obj, through_attr, to_attr
            )

            if not attr_found:
                raise AttributeError(
                    "Cannot find '%s' on %s object, '%s' is an invalid "
                    "parameter to prefetch_related()"
                    % (
                        through_attr,
                        first_obj.__class__.__name__,
                        lookup.prefetch_through,
                    )
                )

            if level == len(through_attrs) - 1 and prefetcher is None:
                # Last one, this *must* resolve to something that supports
                # prefetching, otherwise there is no point adding it and the
                # developer asking for it has made a mistake.
                raise ValueError(
                    "'%s' does not resolve to an item that supports "
                    "prefetching - this is an invalid parameter to "
                    "prefetch_related()." % lookup.prefetch_through
                )

            obj_to_fetch = None
            if prefetcher is not None:
                obj_to_fetch = [obj for obj in obj_list if not is_fetched(obj)]

            if obj_to_fetch:
                obj_list, additional_lookups = prefetch_one_level(
                    obj_to_fetch,
                    prefetcher,
                    lookup,
                    level,
                )
                # We need to ensure we don't keep adding lookups from the
                # same relationships to stop infinite recursion. So, if we
                # are already on an automatically added lookup, don't add
                # the new lookups from relationships we've seen already.
                if not (
                    prefetch_to in done_queries
                    and lookup in auto_lookups
                    and descriptor in followed_descriptors
                ):
                    done_queries[prefetch_to] = obj_list
                    new_lookups = normalize_prefetch_lookups(
                        reversed(additional_lookups), prefetch_to
                    )
                    auto_lookups.update(new_lookups)
                    all_lookups.extend(new_lookups)
                followed_descriptors.add(descriptor)
            else:
                # Either a singly related object that has already been fetched
                # (e.g. via select_related), or hopefully some other property
                # that doesn't support prefetching but needs to be traversed.

                # We replace the current list of parent objects with the list
                # of related objects, filtering out empty or missing values so
                # that we can continue with nullable or reverse relations.
                new_obj_list = []
                for obj in obj_list:
                    if through_attr in getattr(obj, "_prefetched_objects_cache", ()):
                        # If related objects have been prefetched, use the
                        # cache rather than the object's through_attr.
                        new_obj = list(obj._prefetched_objects_cache.get(through_attr))
                    else:
                        try:
                            new_obj = getattr(obj, through_attr)
                        except exceptions.ObjectDoesNotExist:
                            continue
                    if new_obj is None:
                        continue
                    # We special-case `list` rather than something more generic
                    # like `Iterable` because we don't want to accidentally
                    # match user models that define __iter__.
                    if isinstance(new_obj, list):
                        new_obj_list.extend(new_obj)
                    else:
                        new_obj_list.append(new_obj)
                obj_list = new_obj_list


async def aprefetch_related_objects(model_instances, *related_lookups):
    """See prefetch_related_objects()."""
    return await sync_to_async(prefetch_related_objects)(
        model_instances, *related_lookups
    )


def get_prefetcher(instance, through_attr, to_attr):
    """
    For the attribute 'through_attr' on the given instance, find
    an object that has a get_prefetch_querysets().
    Return a 4 tuple containing:
    (the object with get_prefetch_querysets (or None),
     the descriptor object representing this relationship (or None),
     a boolean that is False if the attribute was not found at all,
     a function that takes an instance and returns a boolean that is True if
     the attribute has already been fetched for that instance)
    """

    def is_to_attr_fetched(model, to_attr):
        # Special case cached_property instances because hasattr() triggers
        # attribute computation and assignment.
        if isinstance(getattr(model, to_attr, None), cached_property):

            def has_cached_property(instance):
                return to_attr in instance.__dict__

            return has_cached_property

        def has_to_attr_attribute(instance):
            return hasattr(instance, to_attr)

        return has_to_attr_attribute

    prefetcher = None
    is_fetched = is_to_attr_fetched(instance.__class__, to_attr)

    # For singly related objects, we have to avoid getting the attribute
    # from the object, as this will trigger the query. So we first try
    # on the class, in order to get the descriptor object.
    rel_obj_descriptor = getattr(instance.__class__, through_attr, None)
    if rel_obj_descriptor is None:
        attr_found = hasattr(instance, through_attr)
    else:
        attr_found = True
        if rel_obj_descriptor:
            # singly related object, descriptor object has the
            # get_prefetch_querysets() method.
            if hasattr(rel_obj_descriptor, "get_prefetch_querysets"):
                prefetcher = rel_obj_descriptor
                # If to_attr is set, check if the value has already been set,
                # which is done with has_to_attr_attribute(). Do not use the
                # method from the descriptor, as the cache_name it defines
                # checks the field name, not the to_attr value.
                if through_attr == to_attr:
                    is_fetched = rel_obj_descriptor.is_cached
            else:
                # descriptor doesn't support prefetching, so we go ahead and
                # get the attribute on the instance rather than the class to
                # support many related managers
                rel_obj = getattr(instance, through_attr)
                if hasattr(rel_obj, "get_prefetch_querysets"):
                    prefetcher = rel_obj
                if through_attr == to_attr:

                    def in_prefetched_cache(instance):
                        return through_attr in instance._prefetched_objects_cache

                    is_fetched = in_prefetched_cache
    return prefetcher, rel_obj_descriptor, attr_found, is_fetched


def prefetch_one_level(instances, prefetcher, lookup, level):
    """
    Helper function for prefetch_related_objects().

    Run prefetches on all instances using the prefetcher object,
    assigning results to relevant caches in instance.

    Return the prefetched objects along with any additional prefetches that
    must be done due to prefetch_related lookups found from default managers.
    """
    # prefetcher must have a method get_prefetch_querysets() which takes a list
    # of instances, and returns a tuple:

    # (queryset of instances of self.model that are related to passed in
    #  instances,
    #  callable that gets value to be matched for returned instances,
    #  callable that gets value to be matched for passed in instances,
    #  boolean that is True for singly related objects,
    #  cache or field name to assign to,
    #  boolean that is True when the previous argument is a cache name vs a
    #  field name).

    # The 'values to be matched' must be hashable as they will be used
    # in a dictionary.
    (
        rel_qs,
        rel_obj_attr,
        instance_attr,
        single,
        cache_name,
        is_descriptor,
    ) = prefetcher.get_prefetch_querysets(
        instances, lookup.get_current_querysets(level)
    )
    # We have to handle the possibility that the QuerySet we just got back
    # contains some prefetch_related lookups. We don't want to trigger the
    # prefetch_related functionality by evaluating the query. Rather, we need
    # to merge in the prefetch_related lookups.
    # Copy the lookups in case it is a Prefetch object which could be reused
    # later (happens in nested prefetch_related).
    additional_lookups = [
        copy.copy(additional_lookup)
        for additional_lookup in getattr(rel_qs, "_prefetch_related_lookups", ())
    ]
    if additional_lookups:
        # Don't need to clone because the manager should have given us a fresh
        # instance, so we access an internal instead of using public interface
        # for performance reasons.
        rel_qs._prefetch_related_lookups = ()

    all_related_objects = list(rel_qs)

    rel_obj_cache = {}
    for rel_obj in all_related_objects:
        rel_attr_val = rel_obj_attr(rel_obj)
        rel_obj_cache.setdefault(rel_attr_val, []).append(rel_obj)

    to_attr, as_attr = lookup.get_current_to_attr(level)
    # Make sure `to_attr` does not conflict with a field.
    if as_attr and instances:
        # We assume that objects retrieved are homogeneous (which is the
        # premise of prefetch_related), so what applies to first object applies
        # to all.
        model = instances[0].__class__
        try:
            model._meta.get_field(to_attr)
        except exceptions.FieldDoesNotExist:
            pass
        else:
            msg = "to_attr={} conflicts with a field on the {} model."
            raise ValueError(msg.format(to_attr, model.__name__))

    # Whether or not we're prefetching the last part of the lookup.
    leaf = len(lookup.prefetch_through.split(LOOKUP_SEP)) - 1 == level

    for obj in instances:
        instance_attr_val = instance_attr(obj)
        vals = rel_obj_cache.get(instance_attr_val, [])

        if single:
            val = vals[0] if vals else None
            if as_attr:
                # A to_attr has been given for the prefetch.
                setattr(obj, to_attr, val)
            elif is_descriptor:
                # cache_name points to a field name in obj.
                # This field is a descriptor for a related object.
                setattr(obj, cache_name, val)
            else:
                # No to_attr has been given for this prefetch operation and the
                # cache_name does not point to a descriptor. Store the value of
                # the field in the object's field cache.
                obj._state.fields_cache[cache_name] = val
        else:
            if as_attr:
                setattr(obj, to_attr, vals)
            else:
                manager = getattr(obj, to_attr)
                if leaf and lookup.queryset is not None:
                    qs = manager._apply_rel_filters(lookup.queryset)
                else:
                    qs = manager.get_queryset()
                qs._result_cache = vals
                # We don't want the individual qs doing prefetch_related now,
                # since we have merged this into the current work.
                qs._prefetch_done = True
                obj._prefetched_objects_cache[cache_name] = qs
    return all_related_objects, additional_lookups


class RelatedPopulator:
    """
    RelatedPopulator is used for select_related() object instantiation.

    The idea is that each select_related() model will be populated by a
    different RelatedPopulator instance. The RelatedPopulator instances get
    klass_info and select (computed in SQLCompiler) plus the used db as
    input for initialization. That data is used to compute which columns
    to use, how to instantiate the model, and how to populate the links
    between the objects.

    The actual creation of the objects is done in populate() method. This
    method gets row and from_obj as input and populates the select_related()
    model instance.
    """

    def __init__(self, klass_info, select, db):
        self.db = db
        # Pre-compute needed attributes. The attributes are:
        #  - model_cls: the possibly deferred model class to instantiate
        #  - either:
        #    - cols_start, cols_end: usually the columns in the row are
        #      in the same order model_cls.__init__ expects them, so we
        #      can instantiate by model_cls(*row[cols_start:cols_end])
        #    - reorder_for_init: When select_related descends to a child
        #      class, then we want to reuse the already selected parent
        #      data. However, in this case the parent data isn't necessarily
        #      in the same order that Model.__init__ expects it to be, so
        #      we have to reorder the parent data. The reorder_for_init
        #      attribute contains a function used to reorder the field data
        #      in the order __init__ expects it.
        #  - pk_idx: the index of the primary key field in the reordered
        #    model data. Used to check if a related object exists at all.
        #  - init_list: the field attnames fetched from the database. For
        #    deferred models this isn't the same as all attnames of the
        #    model's fields.
        #  - related_populators: a list of RelatedPopulator instances if
        #    select_related() descends to related models from this model.
        #  - local_setter, remote_setter: Methods to set cached values on
        #    the object being populated and on the remote object. Usually
        #    these are Field.set_cached_value() methods.
        select_fields = klass_info["select_fields"]
        from_parent = klass_info["from_parent"]
        if not from_parent:
            self.cols_start = select_fields[0]
            self.cols_end = select_fields[-1] + 1
            self.init_list = [
                f[0].target.attname for f in select[self.cols_start : self.cols_end]
            ]
            self.reorder_for_init = None
        else:
            attname_indexes = {
                select[idx][0].target.attname: idx for idx in select_fields
            }
            model_init_attnames = (
                f.attname for f in klass_info["model"]._meta.concrete_fields
            )
            self.init_list = [
                attname for attname in model_init_attnames if attname in attname_indexes
            ]
            self.reorder_for_init = operator.itemgetter(
                *[attname_indexes[attname] for attname in self.init_list]
            )

        self.model_cls = klass_info["model"]
        # A primary key must have all of its constituents not-NULL as
        # NULL != NULL and thus NULL cannot be referenced through a foreign
        # relationship. Therefore checking for a single member of the primary
        # key is enough to determine if the referenced object exists or not.
        self.pk_idx = self.init_list.index(self.model_cls._meta.pk_fields[0].attname)
        self.related_populators = get_related_populators(klass_info, select, self.db)
        self.local_setter = klass_info["local_setter"]
        self.remote_setter = klass_info["remote_setter"]

    def populate(self, row, from_obj):
        if self.reorder_for_init:
            obj_data = self.reorder_for_init(row)
        else:
            obj_data = row[self.cols_start : self.cols_end]
        if obj_data[self.pk_idx] is None:
            obj = None
        else:
            obj = self.model_cls.from_db(self.db, self.init_list, obj_data)
            for rel_iter in self.related_populators:
                rel_iter.populate(row, obj)
        self.local_setter(from_obj, obj)
        if obj is not None:
            self.remote_setter(obj, from_obj)


def get_related_populators(klass_info, select, db):
    iterators = []
    related_klass_infos = klass_info.get("related_klass_infos", [])
    for rel_klass_info in related_klass_infos:
        rel_cls = RelatedPopulator(rel_klass_info, select, db)
        iterators.append(rel_cls)
    return iterators
