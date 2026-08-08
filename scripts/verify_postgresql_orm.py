#!/usr/bin/env python3
"""Prove the benchmark shapes enter cppdjango's native ORM terminals."""

import json
import os
import sys


os.environ.setdefault("DJANGO_SETTINGS_MODULE", "hello.settings_local")
os.environ.setdefault("DJANGO_DB", "postgresql")
os.environ.setdefault("BENCH_MW", "orm_isolation")

import django


django.setup()

from django.test import RequestFactory

from world import views
from world.models import World


variant = sys.argv[1] if len(sys.argv) > 1 else "unknown"
counts = {
    "compile_values_list_get": 0,
    "compile_select": 0,
    "compile_query_plan_values": 0,
    "compile_query_plan_update": 0,
    "native_terminal_update": 0,
}
native_available = False
compiler = "stock"
native_handle = False
native_fused = False
native_sql = ""

try:
    from django import native
    from django.native import orm

    native_available = bool(native.AVAILABLE and orm._orm() is not None)
    compiler = native.compiler()
except (ImportError, AttributeError):
    native = None
    orm = None

if native_available:
    for name in (
        "compile_values_list_get",
        "compile_select",
        "compile_query_plan_values",
        "compile_query_plan_update",
    ):
        original = getattr(orm, name)

        def wrapper(*args, __name=name, __original=original, **kwargs):
            counts[__name] += 1
            return __original(*args, **kwargs)

        setattr(orm, name, wrapper)

    from django.db.models import query as query_module

    original_terminal_update = query_module.QuerySet._native_terminal_update

    def terminal_update_wrapper(self, kwargs):
        result = original_terminal_update(self, kwargs)
        if result is not query_module._FAST_PATH_MISS:
            counts["native_terminal_update"] += 1
        return result

    query_module.QuerySet._native_terminal_update = terminal_update_wrapper

    qs = (
        World.objects.filter(id__in=[1, 2, 3])
        .order_by("id")
        .values_list("id", "randomnumber")
    )
    handle = qs._native_handle_for_sql()
    native_handle = handle is not None
    if handle is not None:
        native_sql = handle.compile_sql()[0]
    else:
        from django.db import connection

        compiled = qs._native_compile_deferred_simple_values(connection)
        native_fused = compiled is not None
        if compiled is not None:
            native_sql = compiled[0]
    if not qs._native_authoritative or qs._query is not None:
        raise RuntimeError("supported chain materialized a Python Query")
    if "ORDER BY" not in native_sql:
        raise RuntimeError("native SQL omitted the benchmark ordering")

request_factory = RequestFactory()
for path, view in (
    ("/bench-info", views.bench_info),
    ("/db", views.db),
    ("/dbs?queries=2", views.dbs),
    ("/orm-fortunes", views.orm_fortunes),
    ("/orm-in", views.orm_in),
    ("/orm-update?queries=1", views.orm_update),
):
    response = view(request_factory.get(path))
    if response.status_code != 200:
        raise RuntimeError(f"{path} returned HTTP {response.status_code}")

if variant == "native":
    if not native_available:
        raise RuntimeError("native extension/data plane is unavailable")
    if not (native_handle or native_fused):
        raise RuntimeError("filtered values_list QuerySet has no native execution plan")
    for name, count in counts.items():
        if count < 1:
            raise RuntimeError(f"native ORM probe did not call {name}")
elif native_available:
    raise RuntimeError("stock probe unexpectedly loaded the native extension")

if native_available:
    del qs, handle

print(
    json.dumps(
        {
            "variant": variant,
            "django": django.__version__,
            "django_file": django.__file__,
            "native_available": native_available,
            "compiler": compiler,
            "native_handle": native_handle,
            "native_fused": native_fused,
            "native_sql": native_sql,
            "calls": counts,
        },
        sort_keys=True,
    )
)
