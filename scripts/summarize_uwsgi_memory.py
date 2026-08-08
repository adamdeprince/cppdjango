#!/usr/bin/env python3
"""Summarize stock/cppdjango uWSGI PSS, USS, and prefork COW samples."""

import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
from statistics import median


MEMORY_FIELDS = (
    "rss_kb",
    "pss_kb",
    "uss_kb",
    "proportional_shared_kb",
    "pss_anon_kb",
    "pss_file_kb",
    "private_clean_kb",
    "private_dirty_kb",
    "shared_clean_kb",
    "shared_dirty_kb",
)
COUNTER_FIELDS = ("minor_faults", "major_faults")
PUBLIC_META_FIELDS = (
    "kernel",
    "logical_cpus",
    "memory_kib",
    "transparent_hugepage",
    "ksm_run",
    "postgres",
    "python",
    "psycopg",
    "uwsgi",
    "workers",
    "concurrency",
    "threads",
    "load_kind",
    "repeats",
    "fork_modes",
    "requests_per_endpoint",
    "warmup_requests_per_endpoint",
    "native_base_commit",
    "native_worktree_patch_sha256",
    "harness_sha256",
    "profiler_sha256",
    "mix_script_sha256",
    "siege_rc_sha256",
)


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("raw", type=Path)
    parser.add_argument("load", type=Path)
    parser.add_argument("metadata", type=Path)
    parser.add_argument("--summary", type=Path, required=True)
    parser.add_argument("--snapshots", type=Path, required=True)
    parser.add_argument("--processes", type=Path, required=True)
    return parser.parse_args()


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_metadata(path):
    result = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        for match in re.finditer(
            r"(?:^|\s)([a-z0-9_]+)=(.*?)(?=\s+[a-z0-9_]+=|$)", line
        ):
            result[match.group(1)] = match.group(2)
    return result


def mib(value_kib):
    return round(value_kib / 1024, 2)


def percent_change(before, after):
    return round((after / before - 1) * 100, 2)


def median_value(records, fork_mode, variant, stage, group, field):
    return median(
        record["totals"][group][field]
        for record in records
        if record["fork_mode"] == fork_mode
        and record["variant"] == variant
        and record["stage"] == stage
    )


def median_growth(records, fork_mode, variant, start_stage, end_stage, group, field):
    indexed = {
        (record["repeat"], record["fork_mode"], record["variant"], record["stage"]): record
        for record in records
    }
    repeats = sorted({record["repeat"] for record in records})
    return median(
        indexed[(repeat, fork_mode, variant, end_stage)]["totals"][group][field]
        - indexed[(repeat, fork_mode, variant, start_stage)]["totals"][group][field]
        for repeat in repeats
    )


def write_snapshot_csv(path, records):
    fields = (
        "repeat",
        "variant",
        "fork_mode",
        "stage",
        "group",
        "processes",
        *MEMORY_FIELDS,
        *COUNTER_FIELDS,
    )
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        for record in records:
            for group in ("all", "master", "workers"):
                totals = record["totals"][group]
                writer.writerow(
                    {
                        "repeat": record["repeat"],
                        "variant": record["variant"],
                        "fork_mode": record["fork_mode"],
                        "stage": record["stage"],
                        "group": group,
                        **{field: totals[field] for field in fields if field in totals},
                    }
                )


def write_process_csv(path, records):
    fields = (
        "repeat",
        "variant",
        "fork_mode",
        "stage",
        "role",
        "process_index",
        "threads",
        *MEMORY_FIELDS,
        *COUNTER_FIELDS,
    )
    process_indexes = {}
    for record in records:
        key = (record["repeat"], record["variant"], record["fork_mode"])
        if key not in process_indexes:
            masters = sorted(
                process["pid"] for process in record["processes"] if process["role"] == "master"
            )
            workers = sorted(
                process["pid"] for process in record["processes"] if process["role"] == "worker"
            )
            process_indexes[key] = {
                **{pid: 0 for pid in masters},
                **{pid: index for index, pid in enumerate(workers, 1)},
            }

    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        for record in records:
            key = (record["repeat"], record["variant"], record["fork_mode"])
            for process in record["processes"]:
                writer.writerow(
                    {
                        "repeat": record["repeat"],
                        "variant": record["variant"],
                        "fork_mode": record["fork_mode"],
                        "stage": record["stage"],
                        "role": process["role"],
                        "process_index": process_indexes[key][process["pid"]],
                        "threads": process["threads"],
                        **{field: process[field] for field in (*MEMORY_FIELDS, *COUNTER_FIELDS)},
                    }
                )


def main():
    args = parse_args()
    records = [
        json.loads(line)
        for line in args.raw.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    with args.load.open(encoding="utf-8", newline="") as handle:
        loads = list(csv.DictReader(handle))
    metadata = read_metadata(args.metadata)

    repeats = sorted({record["repeat"] for record in records})
    workers = {record["expected_workers"] for record in records}
    stages = {record["stage"] for record in records}
    if workers != {16}:
        raise ValueError(f"expected only 16-worker records, found {workers}")
    if stages != {"forked", "warmed", "after_equal_work", "settled"}:
        raise ValueError(f"unexpected stages: {stages}")
    if len(records) != len(repeats) * 2 * 2 * len(stages):
        raise ValueError("incomplete memory snapshot matrix")
    request_counts = {int(row["requests"]) for row in loads}
    if len(loads) != len(repeats) * 2 * 2 or len(request_counts) != 1:
        raise ValueError("incomplete or unequal load matrix")
    if any(int(row["errors"]) for row in loads):
        raise ValueError("load errors are present")
    requests = request_counts.pop()

    write_snapshot_csv(args.snapshots, records)
    write_process_csv(args.processes, records)

    result = {
        "schema": 1,
        "benchmark": "cppdjango uWSGI memory and prefork COW comparison",
        "scope": {
            "workers": 16,
            "repeats": len(repeats),
            "measured_requests_per_configuration": requests,
            "request_mix": [
                "one PostgreSQL ORM point select",
                "one ordered 32-row PostgreSQL ORM IN select",
                "one PostgreSQL ORM read plus point update",
            ],
            "load_order": "stock/cppdjango and lazy/preload order alternated by repeat",
            "processes_included": "uWSGI master and workers only; PostgreSQL excluded",
        },
        "definitions": {
            "pss": "Proportional Set Size; shared pages divided among mappings",
            "uss": "Private_Clean plus Private_Dirty; memory unique to each process",
            "private_dirty_growth": (
                "Upper bound on post-fork COW burden; also includes new private allocations"
            ),
            "minor_faults": (
                "Upper-bound COW proxy; also includes demand faults and first-touch allocation"
            ),
            "lazy": "Existing uWSGI lazy-apps mode; Django loads independently in each worker",
            "preload": "Django and the native extension load in the master before workers fork",
        },
        "environment": {key: metadata[key] for key in PUBLIC_META_FIELDS if key in metadata},
        "results": {},
        "copy_on_write": {},
        "preload_savings": {},
        "throughput_context": {},
        "integrity": {
            "raw_jsonl_sha256": sha256(args.raw),
            "load_csv_sha256": sha256(args.load),
            "metadata_sha256": sha256(args.metadata),
        },
    }

    for fork_mode in ("lazy", "preload"):
        mode_result = {}
        for variant in ("stock", "native"):
            all_pss = median_value(
                records, fork_mode, variant, "after_equal_work", "all", "pss_kb"
            )
            all_uss = median_value(
                records, fork_mode, variant, "after_equal_work", "all", "uss_kb"
            )
            worker_pss = median_value(
                records, fork_mode, variant, "after_equal_work", "workers", "pss_kb"
            )
            worker_uss = median_value(
                records, fork_mode, variant, "after_equal_work", "workers", "uss_kb"
            )
            mode_result[variant] = {
                "total_pss_mib": mib(all_pss),
                "total_uss_mib": mib(all_uss),
                "worker_pss_mib": mib(worker_pss),
                "worker_uss_mib": mib(worker_uss),
                "mean_worker_pss_mib": mib(worker_pss / 16),
                "mean_worker_uss_mib": mib(worker_uss / 16),
            }
        stock = mode_result["stock"]
        native = mode_result["native"]
        mode_result["cppdjango_minus_stock"] = {
            "total_pss_mib": round(native["total_pss_mib"] - stock["total_pss_mib"], 2),
            "total_pss_percent": percent_change(
                stock["total_pss_mib"], native["total_pss_mib"]
            ),
            "total_uss_mib": round(native["total_uss_mib"] - stock["total_uss_mib"], 2),
            "total_uss_percent": percent_change(
                stock["total_uss_mib"], native["total_uss_mib"]
            ),
            "pss_mib_per_worker": round(
                (native["total_pss_mib"] - stock["total_pss_mib"]) / 16, 2
            ),
        }
        result["results"][fork_mode] = mode_result

    for variant in ("stock", "native"):
        forked_uss = median_value(
            records, "preload", variant, "forked", "workers", "uss_kb"
        )
        warm_dirty = median_growth(
            records,
            "preload",
            variant,
            "forked",
            "warmed",
            "workers",
            "private_dirty_kb",
        )
        measured_dirty = median_growth(
            records,
            "preload",
            variant,
            "warmed",
            "after_equal_work",
            "workers",
            "private_dirty_kb",
        )
        warm_faults = median_growth(
            records,
            "preload",
            variant,
            "forked",
            "warmed",
            "workers",
            "minor_faults",
        )
        measured_faults = median_growth(
            records,
            "preload",
            variant,
            "warmed",
            "after_equal_work",
            "workers",
            "minor_faults",
        )
        result["copy_on_write"][variant] = {
            "worker_uss_immediately_after_fork_mib": mib(forked_uss),
            "fork_to_warmed_private_dirty_growth_mib": mib(warm_dirty),
            "fork_to_warmed_minor_faults": warm_faults,
            "measured_private_dirty_growth_mib": mib(measured_dirty),
            "measured_private_dirty_kib_per_request": round(measured_dirty / requests, 4),
            "measured_minor_faults": measured_faults,
            "measured_minor_faults_per_request": round(measured_faults / requests, 6),
        }

    stock_cow = result["copy_on_write"]["stock"]
    native_cow = result["copy_on_write"]["native"]
    result["copy_on_write"]["cppdjango_minus_stock"] = {
        "fork_to_warmed_private_dirty_growth_mib": round(
            native_cow["fork_to_warmed_private_dirty_growth_mib"]
            - stock_cow["fork_to_warmed_private_dirty_growth_mib"],
            2,
        ),
        "measured_private_dirty_growth_mib": round(
            native_cow["measured_private_dirty_growth_mib"]
            - stock_cow["measured_private_dirty_growth_mib"],
            2,
        ),
        "measured_minor_faults": (
            native_cow["measured_minor_faults"] - stock_cow["measured_minor_faults"]
        ),
    }

    for variant in ("stock", "native"):
        lazy_pss = median_value(
            records, "lazy", variant, "after_equal_work", "all", "pss_kb"
        )
        preload_pss = median_value(
            records, "preload", variant, "after_equal_work", "all", "pss_kb"
        )
        result["preload_savings"][variant] = {
            "pss_mib": mib(lazy_pss - preload_pss),
            "pss_percent": round((1 - preload_pss / lazy_pss) * 100, 2),
        }

    for fork_mode in ("lazy", "preload"):
        result["throughput_context"][fork_mode] = {}
        for variant in ("stock", "native"):
            rates = [
                float(row["rps"])
                for row in loads
                if row["fork_mode"] == fork_mode and row["variant"] == variant
            ]
            result["throughput_context"][fork_mode][variant] = {
                "median_requests_per_second": round(median(rates), 2),
                "range_requests_per_second": [round(min(rates), 2), round(max(rates), 2)],
            }

    args.summary.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


if __name__ == "__main__":
    main()
