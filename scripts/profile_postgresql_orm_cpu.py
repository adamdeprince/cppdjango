#!/usr/bin/env python3
"""Measure worker-process CPU for the PostgreSQL ORM benchmark shapes.

This script is intended to run from the benchmark application's configured
environment.  In particular, ``DJANGO_SETTINGS_MODULE`` and ``PYTHONPATH``
select either stock Django or cppdjango while the application and database
remain fixed.

The startup delay exists so an external sampling profiler can attach after
Django setup and workload warmup.  CPU and wall counters cover only the
measured loop, not that delay.
"""

import argparse
import gc
import json
import os
from pathlib import Path
import platform
import resource
import time


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "workload",
        choices=(
            "raw_get",
            "orm_get",
            "raw_in32",
            "orm_in32",
            "raw_update",
            "orm_update",
        ),
    )
    parser.add_argument("--variant", required=True)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--duration", type=float, default=8.0)
    group.add_argument("--iterations", type=int)
    parser.add_argument("--warmup", type=int, default=500)
    parser.add_argument("--startup-delay", type=float, default=0.0)
    parser.add_argument("--ready", type=Path)
    parser.add_argument("--result", type=Path)
    return parser.parse_args()


def counter_delta(after, before, attribute, operations):
    return (getattr(after, attribute) - getattr(before, attribute)) / operations


def process_cpu_ticks(pid):
    """Return Linux task user+system ticks, or None when procfs is unavailable."""
    try:
        stat = Path(f"/proc/{pid}/stat").read_text(encoding="ascii")
        # A process name may contain spaces or parentheses. Everything after
        # the final ')' starts at field 3 (state); utime/stime are fields 14/15.
        fields = stat[stat.rfind(")") + 2 :].split()
        return int(fields[11]) + int(fields[12])
    except (IndexError, OSError, ValueError):
        return None


def main():
    args = parse_args()

    import django
    import psycopg

    django.setup()

    from django.db import connection
    from world.models import World

    connection.ensure_connection()
    point_sql = (
        'SELECT "world"."id", "world"."randomnumber" FROM "world" '
        'WHERE "world"."id" = %s LIMIT 21'
    )
    in32_sql = (
        'SELECT "world"."id", "world"."randomnumber" FROM "world" '
        'WHERE "world"."id" IN ('
        + ",".join(["%s"] * 32)
        + ') ORDER BY "world"."id"'
    )
    update_sql = (
        'UPDATE "world" SET "randomnumber" = %s WHERE "world"."id" = %s'
    )

    def raw_get(index):
        row_id = (index * 7919) % 10000 + 1
        with connection.cursor() as cursor:
            cursor.execute(point_sql, [row_id])
            return cursor.fetchone()

    def orm_get(index):
        row_id = (index * 7919) % 10000 + 1
        return World.objects.values_list("id", "randomnumber").get(id=row_id)

    def raw_in32(index):
        start = (index * 97) % 9968 + 1
        identifiers = list(range(start, start + 32))
        with connection.cursor() as cursor:
            cursor.execute(in32_sql, identifiers)
            return cursor.fetchall()

    def orm_in32(index):
        start = (index * 97) % 9968 + 1
        identifiers = list(range(start, start + 32))
        return list(
            World.objects.filter(id__in=identifiers)
            .order_by("id")
            .values_list("id", "randomnumber")
        )

    def raw_update(index):
        row_id = (index * 7919) % 10000 + 1
        value = (index * 3571) % 10000 + 1
        with connection.cursor() as cursor:
            cursor.execute(update_sql, [value, row_id])
            return cursor.rowcount

    def orm_update(index):
        row_id = (index * 7919) % 10000 + 1
        value = (index * 3571) % 10000 + 1
        return World.objects.filter(id=row_id).update(randomnumber=value)

    operation = {
        "raw_get": raw_get,
        "orm_get": orm_get,
        "raw_in32": raw_in32,
        "orm_in32": orm_in32,
        "raw_update": raw_update,
        "orm_update": orm_update,
    }[args.workload]

    result = None
    for index in range(args.warmup):
        result = operation(index)

    postgres_pid = connection.connection.info.backend_pid
    if args.ready:
        args.ready.write_text(
            json.dumps(
                {
                    "worker_pid": os.getpid(),
                    "postgres_pid": postgres_pid,
                },
                sort_keys=True,
            )
            + "\n",
            encoding="utf-8",
        )

    if args.startup_delay:
        time.sleep(args.startup_delay)

    usage_before = resource.getrusage(resource.RUSAGE_SELF)
    postgres_ticks_before = process_cpu_ticks(postgres_pid)
    cpu_before = time.process_time_ns()
    wall_before = time.perf_counter_ns()
    operations = 0
    gc.disable()
    try:
        if args.iterations is not None:
            for index in range(args.iterations):
                result = operation(index + 10000)
            operations = args.iterations
        else:
            deadline = wall_before + int(args.duration * 1_000_000_000)
            index = 10000
            while time.perf_counter_ns() < deadline:
                # Check the deadline once per batch so the timer itself isn't
                # a meaningful part of these sub-millisecond workloads.
                for _ in range(100):
                    result = operation(index)
                    index += 1
                operations += 100
    finally:
        gc.enable()
    wall_after = time.perf_counter_ns()
    cpu_after = time.process_time_ns()
    postgres_ticks_after = process_cpu_ticks(postgres_pid)
    usage_after = resource.getrusage(resource.RUSAGE_SELF)

    wall_seconds = (wall_after - wall_before) / 1_000_000_000
    cpu_seconds = (cpu_after - cpu_before) / 1_000_000_000
    user_seconds = usage_after.ru_utime - usage_before.ru_utime
    system_seconds = usage_after.ru_stime - usage_before.ru_stime
    if postgres_ticks_before is not None and postgres_ticks_after is not None:
        postgres_cpu_seconds = (
            postgres_ticks_after - postgres_ticks_before
        ) / os.sysconf("SC_CLK_TCK")
    else:
        postgres_cpu_seconds = None

    try:
        from django import native

        native_available = bool(native.AVAILABLE)
        native_compiler = native.compiler() if native_available else None
    except (AttributeError, ImportError):
        native_available = False
        native_compiler = None

    record = {
        "variant": args.variant,
        "workload": args.workload,
        "operations": operations,
        "warmup": args.warmup,
        "wall_seconds": wall_seconds,
        "worker_cpu_seconds": cpu_seconds,
        "worker_user_seconds": user_seconds,
        "worker_system_seconds": system_seconds,
        "postgres_cpu_seconds": postgres_cpu_seconds,
        "wall_us_per_operation": wall_seconds / operations * 1_000_000,
        "worker_cpu_us_per_operation": cpu_seconds / operations * 1_000_000,
        "worker_user_us_per_operation": user_seconds / operations * 1_000_000,
        "worker_system_us_per_operation": system_seconds
        / operations
        * 1_000_000,
        "postgres_cpu_us_per_operation": (
            postgres_cpu_seconds / operations * 1_000_000
            if postgres_cpu_seconds is not None
            else None
        ),
        "not_worker_cpu_us_per_operation": (wall_seconds - cpu_seconds)
        / operations
        * 1_000_000,
        "worker_cpu_percent_of_wall": cpu_seconds / wall_seconds * 100,
        "voluntary_context_switches_per_operation": counter_delta(
            usage_after, usage_before, "ru_nvcsw", operations
        ),
        "involuntary_context_switches_per_operation": counter_delta(
            usage_after, usage_before, "ru_nivcsw", operations
        ),
        "minor_faults_per_operation": counter_delta(
            usage_after, usage_before, "ru_minflt", operations
        ),
        "major_faults_per_operation": counter_delta(
            usage_after, usage_before, "ru_majflt", operations
        ),
        "django_version": django.__version__,
        "django_file": django.__file__,
        "psycopg_version": psycopg.__version__,
        "python_version": platform.python_version(),
        "native_available": native_available,
        "native_compiler": native_compiler,
        "prepare_threshold": getattr(
            connection.connection, "prepare_threshold", None
        ),
        "pid": os.getpid(),
        "postgres_pid": postgres_pid,
        "result_type": type(result).__name__,
        "result_size": len(result) if isinstance(result, (list, tuple)) else None,
    }
    rendered = json.dumps(record, sort_keys=True)
    print(rendered, flush=True)
    if args.result:
        args.result.write_text(rendered + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
