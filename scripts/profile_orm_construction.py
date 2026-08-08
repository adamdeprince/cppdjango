#!/usr/bin/env python3
"""Measure construction-only cost for the PostgreSQL benchmark QuerySets."""

import argparse
import gc
import json
import statistics
import timeit


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--variant", required=True)
    parser.add_argument("--iterations", type=int, default=100_000)
    parser.add_argument("--repeats", type=int, default=7)
    return parser.parse_args()


def main():
    args = parse_args()

    import django

    django.setup()

    from world.models import World

    identifiers = list(range(1, 33))
    workloads = {
        "manager_all": lambda: World.objects.all(),
        "values_list": lambda: World.objects.values_list("id", "randomnumber"),
        "point_filter": lambda: World.objects.filter(id=1),
        "ordered_in32_values": lambda: World.objects.filter(
            id__in=identifiers
        ).order_by("id").values_list("id", "randomnumber"),
    }

    # Warm schema registration, imports, and allocator paths before timing.
    for operation in workloads.values():
        for _ in range(1_000):
            operation()

    gc.disable()
    try:
        results = {}
        for name, operation in workloads.items():
            samples = timeit.repeat(
                operation, number=args.iterations, repeat=args.repeats
            )
            sample_us = [value / args.iterations * 1_000_000 for value in samples]
            results[name] = {
                "median_us": statistics.median(sample_us),
                "min_us": min(sample_us),
                "max_us": max(sample_us),
                "samples_us": sample_us,
            }
    finally:
        gc.enable()

    print(
        json.dumps(
            {
                "variant": args.variant,
                "iterations": args.iterations,
                "repeats": args.repeats,
                "django": django.__version__,
                "django_file": django.__file__,
                "results": results,
            },
            sort_keys=True,
        )
    )


if __name__ == "__main__":
    main()
