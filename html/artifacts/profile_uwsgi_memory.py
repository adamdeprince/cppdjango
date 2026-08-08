#!/usr/bin/env python3
"""Capture proportional memory and fault counters for a uWSGI process tree.

Linux ``smaps_rollup`` supplies RSS, PSS, and private/shared page accounting.
PSS is the useful whole-service footprint because summing RSS double-counts
pages shared by the master and workers. USS is derived as Private_Clean plus
Private_Dirty. Absolute minor-fault counters are included so successive
snapshots from the same worker can quantify faults during warmup and load.
"""

import argparse
from datetime import UTC, datetime
import json
import os
from pathlib import Path
import time


ROLLUP_FIELDS = (
    "Rss",
    "Pss",
    "Pss_Dirty",
    "Pss_Anon",
    "Pss_File",
    "Pss_Shmem",
    "Shared_Clean",
    "Shared_Dirty",
    "Private_Clean",
    "Private_Dirty",
    "Referenced",
    "Anonymous",
    "LazyFree",
    "AnonHugePages",
    "Swap",
    "SwapPss",
)


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--master-pid", type=int, required=True)
    parser.add_argument("--expected-workers", type=int, required=True)
    parser.add_argument("--variant", required=True)
    parser.add_argument("--fork-mode", choices=("lazy", "preload"), required=True)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--repeat", type=int, required=True)
    return parser.parse_args()


def read_children(pid):
    path = Path(f"/proc/{pid}/task/{pid}/children")
    text = path.read_text(encoding="ascii").strip()
    return sorted(int(value) for value in text.split()) if text else []


def read_status(pid):
    result = {}
    for line in Path(f"/proc/{pid}/status").read_text(encoding="ascii").splitlines():
        if ":" in line:
            key, value = line.split(":", 1)
            result[key] = value.strip()
    return result


def read_stat(pid):
    text = Path(f"/proc/{pid}/stat").read_text(encoding="ascii")
    command_end = text.rfind(")")
    if command_end < 0:
        raise ValueError(f"malformed /proc/{pid}/stat")
    fields = text[command_end + 2 :].split()
    return {
        "state": fields[0],
        "ppid": int(fields[1]),
        "minor_faults": int(fields[7]),
        "major_faults": int(fields[9]),
        "starttime_ticks": int(fields[19]),
    }


def read_smaps_rollup(pid):
    values = {field: 0 for field in ROLLUP_FIELDS}
    lines = Path(f"/proc/{pid}/smaps_rollup").read_text(encoding="ascii").splitlines()
    for line in lines[1:]:
        if ":" not in line:
            continue
        key, raw_value = line.split(":", 1)
        if key in values:
            values[key] = int(raw_value.split()[0])
    return {f"{key.lower()}_kb": value for key, value in values.items()}


def read_process(pid, role):
    status = read_status(pid)
    record = {
        "pid": pid,
        "role": role,
        "name": status.get("Name", ""),
        "threads": int(status.get("Threads", "0")),
    }
    record.update(read_stat(pid))
    record.update(read_smaps_rollup(pid))
    record["uss_kb"] = record["private_clean_kb"] + record["private_dirty_kb"]
    record["proportional_shared_kb"] = max(record["pss_kb"] - record["uss_kb"], 0)
    return record


def aggregate(records):
    numeric_fields = (
        *(f"{field.lower()}_kb" for field in ROLLUP_FIELDS),
        "uss_kb",
        "proportional_shared_kb",
        "minor_faults",
        "major_faults",
        "threads",
    )
    result = {field: sum(record[field] for record in records) for field in numeric_fields}
    result["processes"] = len(records)
    return result


def read_meminfo():
    wanted = {"MemTotal", "MemFree", "MemAvailable", "Cached", "SwapTotal", "SwapFree"}
    result = {}
    for line in Path("/proc/meminfo").read_text(encoding="ascii").splitlines():
        key, raw_value = line.split(":", 1)
        if key in wanted:
            result[f"{key.lower()}_kb"] = int(raw_value.split()[0])
    return result


def main():
    args = parse_args()
    worker_pids = read_children(args.master_pid)
    if len(worker_pids) != args.expected_workers:
        raise RuntimeError(
            f"expected {args.expected_workers} workers below master {args.master_pid}, "
            f"found {len(worker_pids)}: {worker_pids}"
        )

    master = read_process(args.master_pid, "master")
    workers = [read_process(pid, "worker") for pid in worker_pids]
    processes = [master, *workers]
    record = {
        "schema": 1,
        "timestamp_utc": datetime.now(UTC).isoformat(),
        "monotonic_ns": time.monotonic_ns(),
        "variant": args.variant,
        "fork_mode": args.fork_mode,
        "stage": args.stage,
        "repeat": args.repeat,
        "expected_workers": args.expected_workers,
        "page_size_bytes": os.sysconf("SC_PAGE_SIZE"),
        "clock_ticks_per_second": os.sysconf("SC_CLK_TCK"),
        "system_memory": read_meminfo(),
        "totals": {
            "all": aggregate(processes),
            "master": aggregate([master]),
            "workers": aggregate(workers),
        },
        "processes": processes,
    }
    print(json.dumps(record, sort_keys=True), flush=True)


if __name__ == "__main__":
    main()
