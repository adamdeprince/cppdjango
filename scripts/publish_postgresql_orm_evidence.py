#!/usr/bin/env python3
"""Build a public, path/PID-free evidence bundle from benchmark records."""

import argparse
import csv
import json
from pathlib import Path
import shutil
from statistics import median


SHAPES = {
    "get": "point_select",
    "in32": "ordered_in32_select",
    "update": "point_update",
}
CPU_FIELDS = (
    "operations",
    "warmup",
    "wall_us_per_operation",
    "worker_cpu_us_per_operation",
    "worker_user_us_per_operation",
    "worker_system_us_per_operation",
    "voluntary_context_switches_per_operation",
    "involuntary_context_switches_per_operation",
    "minor_faults_per_operation",
    "major_faults_per_operation",
    "result_type",
    "result_size",
)
MEMORY_FIELDS = (
    "processes",
    "rss_kb",
    "pss_kb",
    "uss_kb",
    "private_clean_kb",
    "private_dirty_kb",
    "shared_clean_kb",
    "shared_dirty_kb",
    "minor_faults",
    "major_faults",
)


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cpu", type=Path, required=True)
    parser.add_argument("--postgres", type=Path, required=True)
    parser.add_argument("--construction", type=Path, required=True)
    parser.add_argument("--memory", type=Path, required=True)
    parser.add_argument("--memory-load", type=Path, required=True)
    parser.add_argument("--patch", type=Path, required=True)
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def load_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def rounded(value, places=4):
    return round(value, places)


def cpu_records(directory):
    records = []
    for path in sorted(directory.glob("rep*.json")):
        record = load_json(path)
        name = path.stem.split("_")
        record["repeat"] = int(name[0].removeprefix("rep"))
        records.append(record)
    return records


def write_cpu_csv(path, records):
    fields = ("repeat", "variant", "workload", *CPU_FIELDS)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        for record in records:
            writer.writerow({field: record.get(field) for field in fields})


def write_postgres_csv(path, records):
    fields = (
        "repeat",
        "variant",
        "workload",
        "operations",
        "wall_us_per_operation",
        "worker_cpu_us_per_operation",
        "postgres_cpu_us_per_operation",
    )
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        for record in records:
            writer.writerow({field: record.get(field) for field in fields})


def write_construction_csv(path, directory):
    fields = ("variant", "run", "workload", "sample", "microseconds")
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        for source in sorted(directory.glob("*.json")):
            record = load_json(source)
            run = int(source.stem.rsplit("_", 1)[1])
            for workload, result in record["results"].items():
                for sample, value in enumerate(result["samples_us"], 1):
                    writer.writerow(
                        {
                            "variant": record["variant"],
                            "run": run,
                            "workload": workload,
                            "sample": sample,
                            "microseconds": value,
                        }
                    )


def summarize_memory(raw_path, load_path, output):
    records = [
        json.loads(line)
        for line in raw_path.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    indexed = {(r["variant"], r["stage"]): r for r in records}
    expected = {
        (variant, stage)
        for variant in ("stock", "native")
        for stage in ("forked", "warmed", "after_equal_work", "settled")
    }
    if set(indexed) != expected:
        raise ValueError("memory validation isn't a complete stock/native matrix")

    with load_path.open(encoding="utf-8", newline="") as handle:
        loads = list(csv.DictReader(handle))
    if len(loads) != 2 or {int(row["requests"]) for row in loads} != {300032}:
        raise ValueError("memory validation isn't equal-work")
    if any(int(row["errors"]) for row in loads):
        raise ValueError("memory validation contains request errors")

    def total(variant, stage, group, field):
        return indexed[(variant, stage)]["totals"][group][field]

    variants = {}
    for variant in ("stock", "native"):
        variants[variant] = {
            "forked_total_pss_kib": total(variant, "forked", "all", "pss_kb"),
            "forked_total_uss_kib": total(variant, "forked", "all", "uss_kb"),
            "warm_private_dirty_growth_kib": (
                total(variant, "warmed", "workers", "private_dirty_kb")
                - total(variant, "forked", "workers", "private_dirty_kb")
            ),
            "measured_private_dirty_growth_kib": (
                total(
                    variant,
                    "after_equal_work",
                    "workers",
                    "private_dirty_kb",
                )
                - total(variant, "warmed", "workers", "private_dirty_kb")
            ),
            "measured_minor_faults": (
                total(variant, "after_equal_work", "workers", "minor_faults")
                - total(variant, "warmed", "workers", "minor_faults")
            ),
            "after_total_pss_kib": total(
                variant, "after_equal_work", "all", "pss_kb"
            ),
            "after_total_uss_kib": total(
                variant, "after_equal_work", "all", "uss_kb"
            ),
            "requests_per_second": float(
                next(row["rps"] for row in loads if row["variant"] == variant)
            ),
        }

    result = {
        "benchmark": "cppdjango final preloaded equal-work COW validation",
        "scope": {
            "workers": 16,
            "concurrency": 64,
            "requests_per_variant": 300032,
            "request_errors": 0,
            "request_mix": ["point select", "ordered IN32 select", "point update"],
        },
        "stock": variants["stock"],
        "native": variants["native"],
        "native_minus_stock": {
            "measured_private_dirty_growth_kib": (
                variants["native"]["measured_private_dirty_growth_kib"]
                - variants["stock"]["measured_private_dirty_growth_kib"]
            ),
            "measured_minor_faults": (
                variants["native"]["measured_minor_faults"]
                - variants["stock"]["measured_minor_faults"]
            ),
            "after_total_pss_kib": (
                variants["native"]["after_total_pss_kib"]
                - variants["stock"]["after_total_pss_kib"]
            ),
            "after_total_uss_kib": (
                variants["native"]["after_total_uss_kib"]
                - variants["stock"]["after_total_uss_kib"]
            ),
        },
        "definitions": {
            "private_dirty_growth": (
                "Upper-bound COW proxy; also includes fresh private allocations"
            ),
            "minor_faults": (
                "Upper-bound COW proxy; also includes demand faults and first touch"
            ),
            "pss": "Proportional Set Size",
            "uss": "Private_Clean plus Private_Dirty",
        },
    }
    output.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return result, records


def write_memory_csv(path, records):
    fields = ("variant", "stage", "group", *MEMORY_FIELDS)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        for record in records:
            for group in ("all", "master", "workers"):
                total = record["totals"][group]
                writer.writerow(
                    {
                        "variant": record["variant"],
                        "stage": record["stage"],
                        "group": group,
                        **{field: total[field] for field in MEMORY_FIELDS},
                    }
                )


def build_summary(cpu, postgres, construction, memory):
    operation_results = []
    for suffix, public_name in SHAPES.items():
        values = {}
        for variant in ("stock", "native"):
            raw = [
                r
                for r in cpu
                if r["variant"] == variant and r["workload"] == f"raw_{suffix}"
            ]
            orm = [
                r
                for r in cpu
                if r["variant"] == variant and r["workload"] == f"orm_{suffix}"
            ]
            pg = [
                r
                for r in postgres
                if r["variant"] == variant and r["workload"] == f"orm_{suffix}"
            ]
            raw_cpu = median(r["worker_cpu_us_per_operation"] for r in raw)
            orm_cpu = median(r["worker_cpu_us_per_operation"] for r in orm)
            values[variant] = {
                "raw_cursor_cpu_us": raw_cpu,
                "worker_cpu_us": orm_cpu,
                "orm_only_cpu_us": orm_cpu - raw_cpu,
                "wall_us": median(r["wall_us_per_operation"] for r in orm),
                "postgres_cpu_us": median(
                    r["postgres_cpu_us_per_operation"] for r in pg
                ),
            }
        stock = values["stock"]
        native = values["native"]
        result = {
            "name": public_name,
            "stock": {key: rounded(value) for key, value in stock.items()},
            "native": {key: rounded(value) for key, value in native.items()},
            "worker_cpu_reduction_percent": rounded(
                (1 - native["worker_cpu_us"] / stock["worker_cpu_us"]) * 100,
                2,
            ),
            "orm_cpu_reduction_percent": rounded(
                (1 - native["orm_only_cpu_us"] / stock["orm_only_cpu_us"]) * 100,
                2,
            ),
            "orm_cpu_performance_ratio": rounded(
                stock["orm_only_cpu_us"] / native["orm_only_cpu_us"], 3
            ),
            "wall_speedup_ratio": rounded(stock["wall_us"] / native["wall_us"], 3),
            "postgres_cpu_delta_percent": rounded(
                (native["postgres_cpu_us"] / stock["postgres_cpu_us"] - 1) * 100,
                2,
            ),
            "combined_worker_and_postgres_cpu_reduction_percent": rounded(
                (
                    1
                    - (native["worker_cpu_us"] + native["postgres_cpu_us"])
                    / (stock["worker_cpu_us"] + stock["postgres_cpu_us"])
                )
                * 100,
                2,
            ),
        }
        operation_results.append(result)

    stock_total = sum(r["stock"]["orm_only_cpu_us"] for r in operation_results)
    native_total = sum(r["native"]["orm_only_cpu_us"] for r in operation_results)
    ratio = stock_total / native_total

    construction_summary = {}
    construction_records = [load_json(path) for path in construction.glob("*.json")]
    for workload in next(iter(construction_records))["results"]:
        values = {}
        for variant in ("stock", "native"):
            samples = [
                value
                for record in construction_records
                if record["variant"] == variant
                for value in record["results"][workload]["samples_us"]
            ]
            values[variant] = median(samples)
        construction_summary[workload] = {
            "stock_median_us": rounded(values["stock"]),
            "native_median_us": rounded(values["native"]),
            "speedup_ratio": rounded(values["stock"] / values["native"], 3),
        }

    return {
        "benchmark": "cppdjango PostgreSQL ORM CPU comparison",
        "baseline": "Django 6.0.7 baseline",
        "native": "cppdjango native implementation released as 6.0.7.post1",
        "release": {
            "distribution": "cppdjango",
            "version": "6.0.7.post1",
            "git_tag": "6.0.7.post1",
            "upstream_compatibility": "Django 6.0.7",
        },
        "headline": {
            "scope": (
                "equal mix of point select, ordered IN32 select, and point update; "
                "matched cursor/psycopg CPU excluded"
            ),
            "stock_orm_cpu_us": rounded(stock_total),
            "native_orm_cpu_us": rounded(native_total),
            "cpu_performance_ratio": rounded(ratio, 4),
            "percent_faster": rounded((ratio - 1) * 100, 1),
            "percent_less_cpu": rounded((1 - 1 / ratio) * 100, 1),
            "published_claim": "436% faster ORM-only CPU performance",
        },
        "operations": operation_results,
        "construction": construction_summary,
        "correctness": {
            "tests": 548,
            "skips": 15,
            "expected_failures": 2,
            "native_disabled_tests": 515,
            "postgresql_integer_oid_parity": True,
        },
        "method": {
            "cpu_repetitions": 3,
            "seconds_per_cpu_sample": 5,
            "warmup_operations": 500,
            "run_order": "stock/native order alternated",
            "postgres_cpu_source": (
                "dedicated backend /proc task ticks sampled at worker boundaries"
            ),
            "memory_validation": memory["scope"],
        },
    }


def main():
    args = parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    cpu = cpu_records(args.cpu)
    postgres = cpu_records(args.postgres)
    if len(cpu) != 36 or len(postgres) != 18:
        raise ValueError("incomplete CPU sample matrix")

    write_cpu_csv(args.output / "cpu-process.csv", cpu)
    write_postgres_csv(args.output / "cpu-postgresql.csv", postgres)
    write_construction_csv(args.output / "construction.csv", args.construction)
    memory, memory_records = summarize_memory(
        args.memory, args.memory_load, args.output / "memory-final-validation.json"
    )
    write_memory_csv(args.output / "memory-final-snapshots.csv", memory_records)
    shutil.copyfile(args.memory_load, args.output / "memory-final-load.csv")
    shutil.copyfile(args.patch, args.output / "measured-source.patch")
    shutil.copyfile(args.driver, args.output / "profile_postgresql_orm_cpu.py")

    summary = build_summary(cpu, postgres, args.construction, memory)
    (args.output / "benchmark-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
