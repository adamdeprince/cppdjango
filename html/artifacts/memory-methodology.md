# PostgreSQL ORM memory and prefork COW benchmark

## Result

At equal work, cppdjango adds **46.85 MiB of PSS across a uWSGI master and 16
workers** in the existing `lazy-apps` configuration: 1,393.68 MiB versus
1,346.83 MiB for stock Django, a **3.48% increase** or about 2.93 MiB per
configured worker.

Loading Django and the native extension before uWSGI forks reduces that gap to
**4.94 MiB total PSS**: 1,213.29 MiB versus 1,208.35 MiB, a **0.41% increase**
or about 0.31 MiB per configured worker. Total unique memory is effectively
identical in this mode: 1,182.24 MiB cppdjango versus 1,182.21 MiB stock.

| uWSGI application loading | Stock PSS | cppdjango PSS | cppdjango change | Stock USS | cppdjango USS |
|---|---:|---:|---:|---:|---:|
| Per-worker (`lazy-apps`) | 1,346.83 MiB | 1,393.68 MiB | **+46.85 MiB / +3.48%** | 1,330.12 MiB | 1,374.16 MiB |
| Master preload, then fork | 1,208.35 MiB | 1,213.29 MiB | **+4.94 MiB / +0.41%** | 1,182.21 MiB | 1,182.24 MiB |

PSS is the correct whole-service comparison because summed RSS counts the same
shared page once for every process mapping it. USS is `Private_Clean +
Private_Dirty` and represents memory unique to a process.

## Copy-on-write result

The C++ port does **not** add a measurable post-fork copy-on-write burden in
this workload. Immediately after a preloaded fork, all 16 workers together use
11.68 MiB of USS for cppdjango and 11.63 MiB for stock. The additional native
code and application state are almost entirely shared at that point.

| Preloaded-worker phase | Stock | cppdjango | cppdjango minus stock |
|---|---:|---:|---:|
| Private-dirty growth, fork to warmed | 437.14 MiB | 437.84 MiB | +0.70 MiB |
| Private-dirty growth, 300,032 measured requests | 713.84 MiB | 713.02 MiB | -0.82 MiB |
| Minor faults during measured requests | 182,745 | 182,535 | -210 |
| Minor faults per measured request | 0.609085 | 0.608385 | -0.000700 |

Private-dirty growth and minor faults are conservative COW proxies, not pure
COW counters: they also include new heap allocations, demand faults, and first
touches. Their native/stock parity is the important result. The port neither
dirties materially more inherited memory nor causes more page-copy faults than
stock Django for the same completed work.

Preloading reduces the post-work PSS footprint by 138.48 MiB (10.28%) for
stock and 180.40 MiB (12.94%) for cppdjango. The larger cppdjango saving is why
its apparent memory premium falls from 3.48% with `lazy-apps` to 0.41% with
preloading.

## Why equal work is required

An initial fixed-24-second diagnostic was intentionally not used for the
memory comparison. The faster native workers completed substantially more
requests in the same time, while the late-run private-memory growth per request
was effectively the same in both lazy variants. Comparing their endpoints at
the same wall time therefore charged cppdjango for doing more useful work.

The authoritative run stops every configuration after exactly 300,032
successful HTTP requests. This makes both the memory endpoint and the fault
delta directly comparable.

## Controlled method

- 16 synchronous uWSGI workers and one master, 64 HTTP/1.1 keep-alive clients.
- Three repetitions with stock/cppdjango and lazy/preload order alternated.
- 15,040 fixed warmup requests followed by exactly 300,032 measured requests
  in every configuration.
- Equal rotation over a point ORM select, an ordered 32-row `IN` ORM select,
  and one ORM read plus point update, all using PostgreSQL.
- The same uWSGI 2.0.31 binary, CPython 3.14.4, psycopg 3.3.4, PostgreSQL 18.4,
  application, data, middleware, client concurrency, and connection settings.
- Zero failed requests, worker deaths, tracebacks, or process-count changes in
  all 12 server lifecycles.
- The 548-test `queries`, `native_orm_dataplane`, and `native_orm_fastpath`
  regression gate passes with 15 skips and two expected failures.

The collector reads `/proc/PID/smaps_rollup` for the master and every worker at
four points:

1. after all workers fork, before the first benchmark request;
2. after the fixed warmup;
3. after the exact measured request count; and
4. after a two-second idle settlement.

It also reads each process's cumulative minor and major faults from
`/proc/PID/stat`. PostgreSQL processes are deliberately excluded, so this
result describes the Python/uWSGI side of the comparison.

The `lazy` leg retains the CPU benchmark's existing `lazy-apps = True` setting:
Django and the extension load independently inside each worker. The `preload`
leg loads the application in the master and then forks. This benchmark opens
database connections only after fork. Applications that create connections,
threads, or other fork-unsafe state during import must correct that lifecycle
before adopting preloading.

## Reproducible artifacts

The public evidence bundle contains:

- `memory-summary.json`: headline medians, paired deltas, definitions, and
  source identities;
- `memory-snapshots.csv`: every aggregate master/worker snapshot;
- `memory-processes.csv`: every anonymized per-process sample;
- `memory-load.csv`: all exact-work load results;
- `profile_uwsgi_memory.py`: the Linux PSS/USS/fault collector;
- `run_uwsgi_postgresql_memory.sh`: the order-alternated harness;
- `uwsgi_memory_mix.lua` and `siege_memory.conf`: the timed diagnostic and
  exact-work client definitions.

The raw public tables use stable process indexes rather than operating-system
PIDs and contain no private hostname or filesystem path.

## Limitations

- The absolute endpoint is memory after a defined request count, not proof of
  an asymptotic allocator plateau. Both implementations retain substantial
  common allocator memory as request count rises.
- Private-dirty pages cannot distinguish copied inherited bytes from fresh
  allocations without intrusive page-table tracing. The paired stock/native
  delta remains valid because the process model and work are controlled.
- The request mix covers the measured PostgreSQL ORM paths. Templates,
  unrelated QuerySet shapes, application caches, and other deployment stacks
  can have different memory behavior.
- Preload savings depend on keeping pre-fork startup free of unsafe database
  connections and threads.
