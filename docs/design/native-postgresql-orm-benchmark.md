# Native PostgreSQL ORM benchmark

The equal-work memory and prefork copy-on-write follow-up is documented in
[`native-postgresql-orm-memory-benchmark.md`](./native-postgresql-orm-memory-benchmark.md).

**Current run:** 2026-08-08
**Host:** the dedicated benchmark machine (Ubuntu 26.04, 128 logical CPUs)
**Release:** cppdjango `6.0.7.post1`, compatible with Django 6.0.7
**Native tree:** cppdjango base `e331be3cef3cd722211caa92e9e02c39c9c7bb9b`
plus the captured hot-path patch listed below
**Baseline:** Django 6.0.7 baseline

The public, path-free records are under [`html/artifacts/`](../../html/artifacts/)
and are served from `https://django.goblinreactor.com/artifacts/`.
Installation instructions are in [`docs/installation.md`](../installation.md)
and at `https://django.goblinreactor.com/install/`.

## Current schema-and-binder result

The current C++ port keeps supported query construction in an immutable,
model-bound `QueryPlan`. Installed-model schema snapshots provide native model
and field IDs, so the three measured terminals no longer call
`model._meta.get_field()`. Exact built-in integer, float, Boolean, character,
and text fields are validated and prepared in C++; custom subclasses,
relations, generated fields, expressions, backend-sensitive types, and values
requiring Django coercion fall back before execution.

PostgreSQL integer width is preserved. The native boundary creates psycopg's
exact `Int2`, `Int4`, or `Int8` parameter object through CPython's C integer
allocator, without executing a Python field hook or Python constructor. A live
server-binding probe verified that the benchmark's `IntegerField` assignment
has the same `int4` OID as the 6.0.7 baseline.

Each value below is the median of three five-second runs after 500 warmup
operations, with stock/native order alternated. “ORM-only” subtracts the
matching implementation's raw Django cursor/psycopg control.

| Operation | Raw cursor CPU, baseline / C++ | Total worker CPU, baseline / C++ | ORM-only CPU, baseline / C++ | ORM-only reduction | Query wall speedup |
|---|---:|---:|---:|---:|---:|
| Point select | 65.44 / 65.59 us | 206.42 / 89.95 us | 140.98 / 24.36 us | **82.7%** | **1.83x** |
| Ordered IN32 | 80.36 / 80.40 us | 276.87 / 113.08 us | 196.51 / 32.68 us | **83.4%** | **1.85x** |
| Point update | 65.31 / 65.08 us | 157.30 / 88.10 us | 91.99 / 23.01 us | **75.0%** | **1.24x** |

Giving each operation one vote produces 429.48 us of baseline ORM-only CPU
versus 80.06 us for cppdjango: **5.36x the work per unit of framework CPU, or
436% faster**. The equivalent consumption statement is **81.4% less ORM
CPU**.

Dedicated PostgreSQL backend CPU was sampled from Linux task counters at the
same measurement boundaries. The baseline/C++ medians were 57.36/56.59 us,
90.71/90.94 us, and 151.03/149.16 us for the three rows above. Every delta is
within 1.4%. PostgreSQL and the common cursor/psycopg floor now account for
84–90% of cppdjango's combined per-query CPU, which explains why the raw wall
speedups are smaller than the framework-only figure.

Construction-only medians also show the effect of keeping registered schema
and replay primitives in C++:

| QuerySet construction | 6.0.7 baseline | cppdjango | Speedup |
|---|---:|---:|---:|
| `objects.all()` | 1.060 us | 0.571 us | 1.86x |
| `values_list(...)` | 16.554 us | 3.737 us | 4.43x |
| `filter(id=...)` | 24.290 us | 3.268 us | 7.43x |
| `filter(id__in=...).order_by(...).values_list(...)` | 67.456 us | 9.100 us | 7.41x |

The regression gate passes 548 native tests with 15 skips and two expected
failures; the meaningful native-disabled fallback gate passes 515 tests. A
final 16-vs-16 preloaded equal-work run completed 300,032 requests per
implementation with zero errors. Native private-dirty growth was 724 KiB
lower and its minor-fault count was 165 lower, which is COW parity rather than
a material memory win.

The sections below retain the earlier endpoint and hardware-counter history.
They are useful context but are not inputs to the current 436% claim.

## Benchmark B result

Benchmark B applies a second optimization pass using Benchmark A's CPU and
boundary evidence. It keeps one safe integer `exact`/`IN` predicate as a
compact immutable descriptor, then fuses filter, ordering, projection, and SQL
compile into one C++ entry at the terminal. The exact-filter update path also
prepares all assignments and compiles the filter and update in one entry.

The controlled setup is identical to Benchmark A. Rates are median HTTP
requests/second across three order-alternated runs.

| Workers | Path | Stock median (range) | Native median (range) | Native/stock |
|---:|---|---:|---:|---:|
| 1 | Plaintext control | 8987.33 (8829.29–9074.42) | 9447.23 (9431.46–9457.82) | 1.051x |
| 1 | One `values_list().get()` | 2297.79 (2285.22–2318.71) | 3410.74 (3409.52–3443.22) | **1.484x** |
| 1 | Twenty `values_list().get()` | 169.91 (163.65–169.98) | 299.41 (292.35–300.99) | **1.762x** |
| 1 | 32-row filtered and ordered `IN` query | 1724.71 (1718.98–1741.97) | 2721.46 (2698.25–2768.87) | **1.578x** |
| 1 | Complete Fortune `values_list()` scan | 2965.94 (2941.53–2970.07) | 3596.41 (3568.83–3648.46) | **1.213x** |
| 1 | Twenty ORM reads + updates | 72.24 (71.91–72.77) | 103.33 (101.66–103.80) | **1.430x** |
| 16 | Plaintext control | 111195.22 (108820.67–111750.09) | 114527.59 (109988.79–119566.21) | 1.030x |
| 16 | One `values_list().get()` | 27534.97 (24196.99–28175.22) | 34405.13 (32545.53–37522.36) | **1.250x** |
| 16 | Twenty `values_list().get()` | 1959.78 (1895.11–2177.03) | 3538.06 (3291.40–3672.58) | **1.805x** |
| 16 | 32-row filtered and ordered `IN` query | 21481.80 (19061.57–22257.18) | 34458.92 (28555.09–34753.93) | **1.604x** |
| 16 | Complete Fortune `values_list()` scan | 38613.02 (35270.17–38812.89) | 35695.50 (34789.85–37672.99) | 0.924x |
| 16 | Twenty ORM reads + updates | 779.06 (751.09–809.31) | 1013.28 (999.37–1034.98) | **1.301x** |

All 72 samples completed with zero load-generator errors, 12 clean uWSGI
lifecycles, all 102 expected worker-ready messages, and no fatal error,
traceback, or worker death. The 16-worker results retain substantial host
variance. In particular, the Fortune endpoint was unchanged by this pass: its
one-worker results reproduced Benchmark A, while its 16-worker stock rate was
24% higher than Benchmark A. That isolated 16-worker reversal should not be
attributed to the fused filter/update change.

At one worker, the targeted native `IN32` rate rose 8.35% over Benchmark A and
the native/stock ratio rose from 1.441x to 1.578x. Native update throughput
rose 4.16%, and its ratio rose from 1.381x to 1.430x. At 16 workers the noisy
`IN32` ratio rose from 1.339x to 1.604x; update's absolute native rate rose
3.42%, while its ratio fell from 1.353x to 1.301x because stock rose more in
that run. The process CPU comparison below is the stronger attribution for
the implementation change.

### Benchmark B process and PostgreSQL CPU

The unprofiled driver, deterministic row sequence, five-second measurement,
500-operation warmup, and raw cursor controls are the same as Benchmark A.

| Path | Raw worker CPU, stock/native | Stock ORM wall / worker CPU | Native ORM wall / worker CPU | Native worker-CPU reduction |
|---|---:|---:|---:|---:|
| Point select | 68.39 / 66.15 us | 252.44 / 205.04 us | 144.60 / 96.75 us | **52.8%** |
| Ordered IN32 select | 80.72 / 79.23 us | 362.98 / 281.23 us | 203.92 / 120.64 us | **57.1%** |
| Point update | 66.73 / 64.43 us | 348.69 / 155.85 us | 290.93 / 98.76 us | **36.6%** |

Relative to Benchmark A's native process counters, worker CPU fell 18.0% for
`IN32` (147.13 to 120.64 us) and 13.9% for update (114.65 to 98.76 us). Their
wall times fell 10.7% and 5.9%, respectively. Point lookup was not targeted;
its 2.9% counter difference is within run-to-run variation.

Subtracting each installation's raw cursor floor leaves ORM-specific worker
CPU of 136.65 us stock versus 30.60 us native for point lookup, 200.51 versus
41.42 us for `IN32`, and 89.12 versus 34.32 us for update. Benchmark B removes
77.6%, 79.3%, and 61.5% of that ORM-only burden, respectively.

Separate `perf stat` attachments to each dedicated PostgreSQL backend show
that the database burden did not change:

| Path | PostgreSQL task CPU, stock/native | PostgreSQL instructions, stock/native | Native/stock instruction delta |
|---|---:|---:|---:|
| Point select | 65.94 / 64.42 us | 160,050 / 160,438 | +0.24% |
| Ordered IN32 select | 97.63 / 99.60 us | 468,466 / 472,481 | +0.86% |
| Point update | 175.39 / 173.55 us | 378,127 / 377,855 | -0.07% |

Worker retired instructions were 811,606 stock versus 327,302 native for a
point lookup, 1,464,834 versus 561,958 for `IN32`, and 549,726 versus 319,945
for update. Compared with Benchmark A's native profiles, the targeted paths
retired 20.3% and 15.1% fewer worker instructions. This agrees with the
process CPU result and rules out a PostgreSQL-side explanation.

### Benchmark B boundary and correctness audit

After schema, dialect, and SQL-shape warmup, a proxy around the bound ORM
module counted these exact steady-state entries:

| ORM operation | Native entries | Entry made |
|---|---:|---|
| `values_list(...).get(id=...)` | **1** | `compile_simple_values_get` |
| `filter(id__in=32).order_by().values_list()` + fetch | **1** | `compile_simple_values_filter` |
| `filter(id=...).update(...)` | **1** | `compile_simple_update` |

There is no C++-to-C++ call routed through Python. Python still owns QuerySet
facade checks, psycopg cursor execution, and result objects. The optimization
freezes a mutable `IN` list into a tuple. It applies only to one concrete,
non-related integer exact/IN predicate; a second predicate, unsupported
lookup, expression, join, inherited model, or introspection request replays
the complete query into stock Django before execution. The regression gate
passes all 541 tests in Django's `queries`, `native_orm_dataplane`, and
`native_orm_fastpath` suites (15 skips and two expected failures).

### Benchmark B artifacts

```text
<benchmark-root>/results/postgres_orm_uwsgi_20260807T164707Z.csv
<benchmark-root>/results/postgres_orm_uwsgi_20260807T164707Z_SUMMARY.md
<benchmark-root>/results/postgres_orm_uwsgi_20260807T164707Z_META.txt
<benchmark-root>/results/postgres_orm_uwsgi_20260807T164707Z_NATIVE.patch
<benchmark-root>/logs/postgres-orm-uwsgi/20260807T164707Z_*.server.log
<benchmark-root>/logs/postgres-orm-uwsgi/20260807T164707Z_*.wrk
<benchmark-root>/results/cpu_profile_b_20260807T171006Z/
```

The captured native patch SHA-256 is
`4916d52c3e596d55952dfcca148899401b47752e63eb24c6e51980479a034aec`.
The CPU directory contains the 12 unprofiled raw/ORM JSON records and matched
worker/PostgreSQL hardware-counter runs. Its captured driver SHA-256 is
`154e85889fb9c99d81fef74350027cd42471f8d44f597ca4c2bd5f5b222f9d62`.

## Benchmark A result

Every measured database endpoint uses Django's PostgreSQL backend and ORM.
Rates are median HTTP requests/second across three order-alternated runs.
Ranges are retained because the 16-worker results show host-level variance.

| Workers | Path | Stock median (range) | Native median (range) | Native/stock |
|---:|---|---:|---:|---:|
| 1 | Plaintext control | 9027.15 (8720.54–9104.66) | 9405.82 (9333.15–9514.10) | 1.042x |
| 1 | One `values_list().get()` | 2317.09 (2307.72–2330.94) | 3397.21 (3382.64–3446.43) | **1.466x** |
| 1 | Twenty `values_list().get()` | 168.38 (167.45–169.38) | 295.48 (294.69–298.68) | **1.755x** |
| 1 | 32-row filtered and ordered `IN` query | 1743.03 (1726.19–1768.49) | 2511.79 (2495.79–2529.55) | **1.441x** |
| 1 | Complete Fortune `values_list()` scan | 2966.14 (2880.39–2980.73) | 3628.86 (3581.05–3635.18) | **1.223x** |
| 1 | Twenty ORM reads + updates | 71.84 (71.70–72.84) | 99.20 (96.94–99.53) | **1.381x** |
| 16 | Plaintext control | 112432.94 (112096.81–116547.10) | 114532.53 (114193.52–114989.30) | 1.019x |
| 16 | One `values_list().get()` | 26533.21 (23916.77–30467.63) | 34290.87 (33716.64–35723.94) | **1.292x** |
| 16 | Twenty `values_list().get()` | 2116.70 (1914.92–2169.44) | 3270.07 (3148.94–3788.08) | **1.545x** |
| 16 | 32-row filtered and ordered `IN` query | 19289.53 (18846.54–20833.84) | 25832.83 (24255.14–29596.68) | **1.339x** |
| 16 | Complete Fortune `values_list()` scan | 31215.73 (30418.29–31370.89) | 37089.53 (35419.26–38423.68) | **1.188x** |
| 16 | Twenty ORM reads + updates | 724.27 (722.97–729.95) | 979.81 (958.03–1140.09) | **1.353x** |

There were 72 measured samples, zero load-generator errors, and 12 clean
uWSGI lifecycles. All expected workers became ready, and the server logs
contain no Python fatal error, traceback, or worker death.

The plaintext control is 4.2% faster at one worker and 1.9% faster at 16
workers. Dividing the ORM ratios by that control gives an approximate
ORM-specific improvement of 1.17x–1.68x at one worker and 1.17x–1.52x at 16
workers. This normalization is descriptive, not a substitute for the raw
controlled comparison.

## Benchmark A correctness correction

The earlier `20260807T043052Z` run is superseded for the `IN` endpoint. The
Python query contained `ORDER BY`, but the mirrored C++ graph omitted it, so
stock and native executed different SQL. The new authoritative native graph
emits:

```sql
SELECT "world"."id" AS "id",
       "world"."randomnumber" AS "randomnumber"
FROM "world"
WHERE "world"."id" IN (%s, %s, %s)
ORDER BY "world"."id"
```

The verification probe rejects a run if the native SQL lacks `ORDER BY` or
if the supported chain has already populated the Python `WhereNode`.
Unsupported joins, transforms, inherited models, related ordering, and
nullable negation fall back to Django before execution. This conservative
gate was checked against all 495 tests in Django's `queries` suite.

## Benchmark A hot-path changes

The rebenchmark includes five related changes:

1. Installed model schemas are exported after app startup. Model IDs and
   connection dialects are cached, with generation invalidation after an
   explicit schema clear.
2. Simple projection/get and projection/select shapes compile in one native
   call. Filtered updates pass the complete prepared assignment map to C++ in
   one call.
3. Supported `filter → order_by → values` chains keep the C++ graph
   authoritative. A compact replay log materializes a Python `Query` only for
   introspection or fallback; the two graphs are no longer built in parallel.
4. Slice, filter-argument, sticky-chain, and ordering checks stay in Python
   instead of making one-operation native calls. Python queryset clones share
   their native wrapper, while functional native mutations return a COW child.
5. Native query state is copy-on-write. Compiled SQL is cached per state and
   invalidated by mutation or schema generation; simple terminal SQL shapes
   also have a generation-aware process cache.

The compiler also avoids copying an existing native SELECT vector during SQL
emission. Case-insensitive `iexact` is no longer mislabeled as plain equality;
it falls back until the compiler has backend-correct case-insensitive
semantics.

## Benchmark A Python/C++ boundary audit

A proxy around the bound ORM module counted steady-state native API entries on
the exact benchmark models:

| ORM operation | Native entries | Entries made |
|---|---:|---|
| `values_list(...).get(id=...)` | **1** | fused cached compile |
| `filter(id__in=32).order_by().values_list()` + compile | **4** | create-from-Q, functional ordering, functional projection, compile |
| `filter(id=...).update(...)` | **2** | create-from-Q, compile all assignments |

Before these changes the corresponding point-get path made eight native
entries, while construction and compilation of the ordered `IN` shape made
26. C++-to-C++ operations now remain inside the bound call; the implementation
does not route C++ methods back through the Python interpreter. Python still
owns field-value preparation, the PostgreSQL cursor call, and row-to-Python
materialization, as intended by this benchmark's scope.

The diagnosis was therefore primarily excessive orchestration across the
language boundary and duplicate query construction, not a slow core C++ SQL
emitter. COW state and compile caching remove the remaining repeated native
graph copies and SQL rendering.

## Benchmark A worker and PostgreSQL CPU attribution

A follow-up run on the same host separated worker-process CPU from elapsed
time and profiled the dedicated PostgreSQL backend process. Each steady-state
run used one persistent connection, the same deterministic row sequence, a
500-operation warmup, and five seconds of unprofiled measurement. The raw SQL
variants provide the common Django cursor, psycopg, TLS, and row-decoding
floor. `RUSAGE_SELF` and the process CPU clock exclude time for which the
worker was blocked or descheduled.

| Path | Raw worker CPU, stock/native | Stock ORM wall / worker CPU | Native ORM wall / worker CPU | Native worker-CPU reduction |
|---|---:|---:|---:|---:|
| Point select | 65.90 / 66.06 us | 263.69 / 211.41 us | 147.78 / 99.65 us | **52.9%** |
| Ordered IN32 select | 79.65 / 80.09 us | 372.84 / 285.29 us | 228.41 / 147.13 us | **48.4%** |
| Point update | 63.79 / 63.72 us | 351.14 / 158.15 us | 309.08 / 114.65 us | **27.5%** |

The wall time not charged to the worker process was 48-52 us for a point
select, 81-88 us for IN32, and 192-194 us for an autocommit update. This is a
wait category, not a PostgreSQL CPU category: it includes server execution,
storage and scheduler waits, while client and server CPU can overlap on
different cores.

Linux `perf stat` was therefore attached separately to the worker and to its
dedicated `pg_backend_pid()`. PostgreSQL instructions per operation are the
most stable comparison because task-clock time varies with core frequency.

| Path | PostgreSQL task CPU, stock/native | PostgreSQL instructions, stock/native | Native/stock instruction delta |
|---|---:|---:|---:|
| Point select | 70.22 / 64.82 us | 160,077 / 160,574 | +0.3% |
| Ordered IN32 select | 100.14 / 98.85 us | 468,974 / 473,419 | +0.9% |
| Point update | 174.12 / 171.82 us | 370,188 / 369,084 | -0.3% |

PostgreSQL is doing the same work for both implementations to within 1% of
retired instructions. Combining the separately measured worker and server
task clocks gives an approximate native CPU split of 61% worker / 39%
PostgreSQL for a point select, 60% / 40% for IN32, and 40% / 60% for an
update. For stock Django those splits are 75% / 25%, 74% / 26%, and 48% /
52%, respectively. These sums describe CPU demand, not request wall time.

`EXPLAIN (ANALYZE, TIMING OFF)` reports median plan-plus-executor times of 22
us for the point select, 46 us for IN32, and 29 us for the update itself. The
larger backend task-clock values also include PostgreSQL protocol and TLS work,
transaction lifecycle, and, for updates, WAL processing. `EXPLAIN` does not
charge the autocommit flush to the update executor. The benchmark connection
uses TLS 1.3 even on loopback, identically for both variants.
Django's default psycopg `ClientCursor` is retained. Consequently,
`prepare_threshold=0` alone doesn't create server-side prepared statements;
`server_side_binding=True` would also be required, and planning remains part
of both measured legs.

The raw worker floor is also identical across the installations. Subtracting
it leaves ORM-specific worker CPU of 145.51 us stock versus 33.58 us native
for a point select, 205.65 versus 67.04 us for IN32, and 94.36 versus 50.93
us for an update. The native changes therefore remove 77%, 67%, and 46% of
the corresponding ORM-only CPU burden.

CPU-clock sampling reinforces that result. In the native profiles,
`_native.so` plus `libstdc++` accounted for about 1.0% of point-select worker
samples, 5.4% of IN32 samples, and 2.9% of update samples. The CPython
executable accounted for 70.9%, 69.0%, and 71.5%. GIL-held Python-stack
samples show the remaining native time concentrated in the QuerySet facade
and psycopg; stock Django additionally spends substantial CPU in
`sql/query.py`, lookups, and `SQLCompiler`. The higher psycopg percentage in
the native profile is denominator shrinkage: its absolute raw-path CPU did
not change.

This makes the current limit unambiguous: the C++ SQL engine is not consuming
much CPU, and the database is not becoming cheaper on the native path. The
speedup comes from removing Python ORM/compiler work. The remaining select
overhead is mostly common psycopg/TLS work plus Python facade and result
handling; update throughput is now primarily limited by PostgreSQL transaction
and WAL work.

The complete evidence is on the dedicated benchmark machine:

```text
<benchmark-root>/results/cpu_profile_20260807T160312Z/
```

It contains the unprofiled JSON runs, worker and PostgreSQL `perf stat`
counters, six CPU-clock `perf.data` files, six GIL-held Python stack profiles,
and the exact driver script. The directory has 69 files (97 MiB). Driver
SHA-256:

```text
154e85889fb9c99d81fef74350027cd42471f8d44f597ca4c2bd5f5b222f9d62
```

The sampler was installed in a separate `venvs/profiler` environment; neither
the stock nor native benchmark environment was modified.

## Workload paths

| Endpoint | ORM operation per request |
|---|---|
| `/db` | One `World.objects.values_list(...).get(id=...)` |
| `/dbs?queries=20` | Twenty independent `values_list().get()` calls |
| `/orm-in` | `filter(id__in=32 ids).order_by("id").values_list()` |
| `/orm-fortunes` | Complete `Fortune.objects.values_list()` scan; JSON response |
| `/orm-update?queries=20` | Twenty ORM reads and twenty `filter(id=...).update()` calls |
| `/plaintext` | No database work; non-ORM control |

The update endpoint contains no raw cursor SQL. The Fortune endpoint returns
JSON rather than using cppdjango's native Fortune HTML helper, so those
results measure the ORM scan and shared response construction.

## Benchmark A native-path proof

Before load generation, `verify_postgres_orm.py` executes every shape and
checks the authoritative graph and SQL. The official run recorded:

```text
stock:  native_available=false; native compiler calls=0
native: native_available=true; live native QuerySet handle=true
native calls: compile_values_list_get=4, compile_select=1,
              successful native_terminal_update=1
native SQL: ... WHERE "world"."id" IN (%s, %s, %s)
            ORDER BY "world"."id"
```

The worker identity endpoint independently recorded upstream Django from the
stock virtual environment and cppdjango from
`<benchmark-root>/src/cppdjango`.

## Controlled methodology

Held constant:

- CPython 3.14.4 and psycopg 3.3.4.
- Local PostgreSQL 18.4, with 10,000 World rows and 12 Fortune rows.
- The app, PostgreSQL data, connection settings, response construction, and
  `/usr/bin/wrk` load generator.
- uWSGI 2.0.31 and one shared configuration. Binary SHA-256:
  `418909d08d55af4c57103054ef0a7e98ba5c8333cc97b4db05fa3b2cddb6e481`.
- Three-second endpoint warmup followed by a 15-second measurement.
- Three repetitions, alternating stock/native server order.
- 16 client connections for one worker and 64 for 16 workers.
- A custom pass-through middleware that disables cppdjango's native outer
  WSGI loop and sets `Content-Length` identically for both variants.

Varied only the Django implementation and native state:

- Stock: Django 6.0.7 baseline, with no `django.native` package.
- Native: cppdjango 6.0.7, `DJANGO_NATIVE=1`, extension compiled by g++ 15.2.0.

The benchmark harness now captures the native base commit, dirty-tree patch
and SHA-256, plus harness and verification-probe hashes in the metadata for
future runs.

## uWSGI settings

The CPython 3.14 fork fix remains required:

```ini
master = True
lazy-apps = True
py-call-uwsgi-fork-hooks = True
```

The load generator also requires uWSGI's HTTP/1.1 socket:

```ini
http11-socket = 127.0.0.1:8080
```

Using `http-socket` closed every connection after a response. `http11-socket`,
together with an explicit content length, produced persistent connections and
zero transport errors.

## Benchmark A artifacts on the dedicated benchmark machine

```text
<benchmark-root>/results/postgres_orm_uwsgi_20260807T130404Z.csv
<benchmark-root>/results/postgres_orm_uwsgi_20260807T130404Z_SUMMARY.md
<benchmark-root>/results/postgres_orm_uwsgi_20260807T130404Z_META.txt
<benchmark-root>/results/postgres_orm_uwsgi_20260807T130404Z_NATIVE.patch
<benchmark-root>/logs/postgres-orm-uwsgi/20260807T130404Z_*.server.log
<benchmark-root>/logs/postgres-orm-uwsgi/20260807T130404Z_*.wrk
```

Captured native patch SHA-256:

```text
162e7aed00f4b0e3bd09c374ac951a8508d51028c5142db2f30abd3bc31a795a
```

That is the exact Benchmark A patch. Its post-run tree additionally routed an
explicit `compile_select(limit=0)` around the simple-shape cache (and added its
regression test); none of the benchmark endpoints supplies that argument, so
the guard does not affect Benchmark A's reported paths.

## Reproduction

```bash
# Official defaults: 15-second samples, 3-second warmup, 3 repetitions,
# and both 1-worker and 16-worker legs.
<benchmark-root>/run_uwsgi_postgres_orm.sh

# Short smoke.
DURATION=2 WARMUP=1 REPEATS=1 WORKERS=1 \
  <benchmark-root>/run_uwsgi_postgres_orm.sh
```

The harness, app endpoints, verification probe, uWSGI profile, and summarizer
are under `<benchmark-root>/`.
