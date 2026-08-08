# Native DCPerf benchmark methodology and uWSGI/CPython 3.14 fix

> **Scope warning:** this DCPerf workload uses Cassandra and does not measure
> cppdjango's PostgreSQL ORM data plane. Use
> [`native-postgresql-orm-benchmark.md`](./native-postgresql-orm-benchmark.md)
> for the PostgreSQL ORM comparison. The material below remains useful as a
> service-stack control and as documentation of the uWSGI fork-hook failure.

**Audience:** engineers reproducing the cppdjango vs Django DCPerf comparison.
**Updated:** 2026-08-07
**Host of record:** the dedicated benchmark machine (Ubuntu 26.04, Threadripper PRO 5995WX, 128 logical CPUs).
**Product revision:** cppdjango `e331be3cef`; upstream baseline Django 6.0.7
`e2a424605a`.

---

## 1. Result

The fair 128-worker uWSGI comparison now works on CPython 3.14.4.

Run: `2M` per iteration, three iterations per leg. The DCPerf parser removes
the minimum and maximum transaction-rate iterations and reports the remaining
sample.

| Leg | Retained rate | Availability | Failed | Mean / P99 response |
|---|---:|---:|---:|---:|
| Upstream Django 6.0.7 | **4497.97 tps** | 100% | 0 | 0.03 s / 0.08 s |
| cppdjango 6.0.7 native | **4474.53 tps** | 100% | 0 | 0.03 s / 0.08 s |

Native/stock = **0.9948x**, or **-0.52%**. Treat this as parity for this
service-heavy workload, not as a framework-floor speedup. DCPerf uses custom
middleware, Cassandra, memcached, and `icachebuster`; its middleware stack is
not eligible for cppdjango's outer native WSGI loop.

Result logs:

```text
<dcperf-root>/results/standalone_2Mx3_stock_20260807T031102Z.log
<dcperf-root>/results/standalone_2Mx3_native_20260807T031745Z.log
```

Raw transaction rates observed before min/max removal:

```text
stock:  4507.46, 4497.97, 4474.17 tps
native: 4346.92, 4494.73, 4474.53 tps
```

---

## 2. What “stock” and “native” mean

| Label | Definition |
|---|---|
| **Fair stock** | Django 6.0.7 baseline source at `e2a424605a`, CPython 3.14.4, uWSGI 2.0.31 |
| **Fair native** | cppdjango source at `e331be3cef`, `DJANGO_NATIVE=1`, CPython 3.14.4, uWSGI 2.0.31 |
| **Fallback control** | cppdjango source with `DJANGO_NATIVE=0`; useful for debugging, but not the official stock baseline |
| **Legacy DCPerf** | Packaged Django 4.1 / CPython 3.10 / original uWSGI profile; not a valid dual-path A/B baseline |

The earlier “fair stock” definition used the modified cppdjango tree with
`DJANGO_NATIVE=0`. That exercises the Python fallbacks, but cppdjango changes
many Django modules and is not as strong a baseline as stock baseline
source. The current stock leg has no `django.native` package at all.

---

## 3. Paths on the dedicated benchmark machine

| Item | Path |
|---|---|
| DCPerf root | `<dcperf-root>` |
| Native dependency venv | `<dcperf-root>/venvs/django_workload_native` |
| Stock dependency venv | `<dcperf-root>/venvs/django_workload_stock` |
| Upstream 6.0.7 worktree | `<dcperf-root>/stock/django-6.0.7` |
| CPython 3.14 cache-client overlay | `<dcperf-root>/deps/pylibmc2-1.6.4` |
| Workload app | `<dcperf-root>/benchmarks/django_workload/django-workload/django-workload` |
| Harness | `<dcperf-root>/benchmarks/django_workload/bin/run.sh` |
| Shared uWSGI profile | workload app `uwsgi_fair.ini` |
| Durable DCPerf templates | `<dcperf-root>/packages/django_workload/templates/{run.sh,uwsgi.ini,uwsgi_fair.ini}` |
| Wrappers | `<dcperf-root>/scripts/run_{stock,native,fair_compare}.sh` |
| Results | `<dcperf-root>/results/` |
| Framework-floor benches | `<benchmark-root>/` |

The stock venv was cloned from the native dependency venv without
`_editable_skbc_Django.{pth,py}`. `DJANGO_SOURCE_ROOT` then places the upstream
worktree on `PYTHONPATH`. Both legs explicitly execute the same uWSGI binary:

```text
<dcperf-root>/venvs/django_workload_native/bin/uwsgi
sha256 418909d08d55af4c57103054ef0a7e98ba5c8333cc97b4db05fa3b2cddb6e481
```

---

## 4. Controlled comparison

Held constant:

- Host, kernel, CPython 3.14.4, app, and dependency versions.
- uWSGI 2.0.31 binary and `uwsgi_fair.ini`.
- 128 sync workers and a 511-connection listen queue.
- Cassandra/memcached configuration and app setup path.
- The CPython-3.14-compatible `pylibmc2` 1.6.4 overlay.
- Siege client count (154), URL weights, duration, and iterations.
- `DCPERF_RANDOM_SEED=20260806` and `PYTHONHASHSEED=0`.
- A fresh memcached process/empty cache for each leg.

Varied:

- Upstream Django source vs cppdjango source.
- Native extension absent vs available/enabled.

The harness records the resolved Django and cache-client source paths beside
the server launch. This makes accidental editable-install leakage visible in
the result log.

### Commands

```bash
# Official comparison (defaults are also 2M x 3).
DURATION=2M ITERATIONS=3 <dcperf-root>/scripts/run_fair_compare.sh

# Short end-to-end smoke.
DURATION=30S ITERATIONS=1 <dcperf-root>/scripts/run_fair_compare.sh

# Individual legs.
DURATION=2M ITERATIONS=3 <dcperf-root>/scripts/run_stock.sh
DURATION=2M ITERATIONS=3 <dcperf-root>/scripts/run_native.sh

# Smaller worker count for debugging only.
DJANGO_SERVER_WORKERS=8 DURATION=10S ITERATIONS=1 \
  <dcperf-root>/scripts/run_fair_compare.sh
```

Do not use `ITERATIONS=2`. This parser removes the min and max sample, leaving
no metric. The wrappers now reject it. Use one iteration for smoke or at least
three for a measured result.

### Harness lifecycle

For each standalone leg, the harness:

1. Starts Cassandra and a fresh memcached process.
2. Runs Django `flush` and deterministic `setup`.
3. Starts uWSGI and waits for HTTP 200 from `/timeline`.
4. Generates a deterministically shuffled URL file.
5. Runs Siege for the requested duration and iterations.
6. Gracefully stops the uWSGI master, then cleans up all services.

The memcached launcher must `exec` the daemon. Without that, killing the shell
wrapper leaves a warmed child process behind and biases the following leg.

---

## 5. uWSGI + CPython 3.14 root cause and fix

### Symptom

With the old profile, every worker aborted immediately:

```text
Fatal Python error: PyMutex_Unlock: unlocking mutex that is not locked
DAMN ! worker N ... died, killed by signal 6 :( trying respawn ...
```

This reproduced with a trivial WSGI lambda, so it was neither Django nor the
cppdjango extension.

### Proven root cause

The profile enabled:

```ini
py-call-osafterfork = True
```

In uWSGI 2.0.31, that legacy option calls `PyOS_AfterFork_Child()` in the child
without arranging a matching `PyOS_BeforeFork()` in the parent. CPython 3.14's
runtime lock checks detect the unbalanced unlock and abort.

Controlled trivial-app reproduction on the dedicated benchmark machine:

| Fork mode | HTTP | Live workers | Fatal errors |
|---|---:|---:|---:|
| `--py-call-osafterfork` | 000 | respawn loop | 6 during probe |
| `--py-call-uwsgi-fork-hooks` | 200 | 2/2 | 0 |
| no explicit hook | 200 | 2/2 | 0 |

The supported bracketed uWSGI path is:

```ini
py-call-uwsgi-fork-hooks = True
```

uWSGI then invokes `PyOS_BeforeFork()`, `PyOS_AfterFork_Parent()`, and
`PyOS_AfterFork_Child()` around its fork.

### Shared fair profile

Relevant `uwsgi_fair.ini` settings:

```ini
[uwsgi]
http-socket = 0.0.0.0:8000
wsgi-file = django_workload/wsgi.py
wsgi-env-behavior = holy
master = True
lazy-apps = True
py-call-uwsgi-fork-hooks = True
thunder-lock = True
need-app = True
```

Additional harness corrections:

- Use the default pthread robust lock engine; do not use persistent SysV
  semaphores that survive interrupted runs.
- Do not export an environment variable named `UWSGI_INI`. uWSGI consumes
  `UWSGI_*` environment variables as options, which made it load the profile
  once from the environment and again from `--ini`.
- Use `--pidfile`, not `--safe-pidfile`; the latter was overwritten by lazy
  workers, so cleanup sometimes signaled a worker instead of the master.
- Load apps after fork (`lazy-apps`) so Cassandra connections are worker-local.

Full validation: 128/128 workload apps ready, three 2-minute iterations per
leg, no `PyMutex` fatal, worker death, traceback, or readiness failure.

---

## 6. CPython 3.14 memcached client

The packaged `pylibmc` 1.6.3 release predates CPython 3.14. A single client
construction produced 155 lines of ignored `PyMapping_HasKeyString()` errors.
Those tracebacks polluted the server log and materially distorted the request
path even though cache get/set eventually succeeded.

Both fair legs therefore prepend the same `pylibmc2` 1.6.4 wheel overlay. It
provides the same `pylibmc` API and publishes a CPython 3.14 wheel.

Probe result:

```text
pylibmc  1.6.3: set/get passed, 155 stderr lines
pylibmc2 1.6.4: set/get passed,   0 stderr lines
```

Do not compare a result using 1.6.3 with one using 1.6.4.

---

## 7. Interpretation

The measured DCPerf result is parity, with native 0.52% below stock. This is
consistent with the workload architecture:

- Custom Messages/application middleware keeps `_use_native_wsgi_outer` false.
- Cassandra, cache access, response payload work, and `icachebuster` dominate.
- Native acceleration still affects eligible utility/data-plane calls, but the
  framework request-loop win measured by a minimal app is not available here.

The PostgreSQL-only ORM comparisons are documented in
[`native-postgresql-orm-benchmark.md`](./native-postgresql-orm-benchmark.md).
The official `20260807T130404Z` run is **Benchmark A**. It has 72 samples and
zero errors, and its corrected ordered-`IN` SQL includes `ORDER BY`.

Benchmark A's process and PostgreSQL profiles showed that the remaining cost
was in Python QuerySet orchestration rather than the C++ SQL emitter or a
different database plan. That evidence drove **Benchmark B**: a safe single
integer exact/IN descriptor and fused terminal compilers reduce each measured
point, IN32, and update shape to one steady-state native entry. Multi-filter
and unsupported shapes replay into stock Django before execution.

The official Benchmark B uWSGI run is `20260807T164707Z`. At one worker,
native/stock is 1.484x for a point lookup, 1.762x for query20, 1.578x for
ordered IN32, 1.213x for Fortune, and 1.430x for update20. At 16 workers the
corresponding ratios are 1.250x, 1.805x, 1.604x, 0.924x, and 1.301x; retain
the documented ranges because those samples have substantial host variance.
All 72 samples completed without errors or worker deaths.

Matched Benchmark B process counters put native worker CPU at 96.75 us for a
point lookup, 120.64 us for IN32, and 98.76 us for update. The targeted IN32
and update paths use 18.0% and 13.9% less worker CPU than Benchmark A.
PostgreSQL retired instructions remain matched within 0.9% between stock and
native. Benchmark A's complete stack profiles are under
`<benchmark-root>/results/cpu_profile_20260807T160312Z/`; Benchmark B's
matched process and hardware-counter records are under
`<benchmark-root>/results/cpu_profile_b_20260807T171006Z/`.

Use `<benchmark-root>/` and `docs/design/native-wsgi-handler.md` for
framework-floor claims. Use this DCPerf comparison for realistic service-stack
behavior. Do not combine a Django 4.1/uWSGI result with a Django 6.0.7/gunicorn
or cppdjango result and call the ratio a native speedup.

---

## 8. Legacy baseline

The original packaged benchmark remains available:

```bash
USE_LEGACY_STOCK=1 DURATION=2M ITERATIONS=3 \
  <dcperf-root>/scripts/run_stock.sh
```

It uses Django 4.1, CPython 3.10, and the corrected packaged `uwsgi.ini`. It answers
“how does the historical DCPerf package run?” but is not the stock side of the
cppdjango A/B.

---

## 9. Quick validation checklist

```bash
# Both identities must resolve to different, expected source trees.
PYTHONPATH=<dcperf-root>/stock/django-6.0.7 \
  <dcperf-root>/venvs/django_workload_stock/bin/python -c \
  'import django; print(django.__version__, django.__file__)'

<dcperf-root>/venvs/django_workload_native/bin/python -c \
  'import django; from django import native; print(django.__file__, native.AVAILABLE)'

# The two copied uWSGI executables should be identical; wrappers explicitly use
# the native path for both regardless.
cmp <dcperf-root>/venvs/django_workload_{stock,native}/bin/uwsgi

# After a run there should be no benchmark listeners or workers.
ss -ltn '( sport = :8000 or sport = :9042 or sport = :11811 )'
```

Expected result-log identity lines:

```text
stock:  Django runtime: 6.0.7 .../DCPerf/stock/django-6.0.7/django/__init__.py
native: Django runtime: 6.0.7 .../cppdjango/django/__init__.py
both:   Cache runtime: 1.6.4 .../DCPerf/deps/pylibmc2-1.6.4/...
both:   Starting uWSGI binary=.../django_workload_native/bin/uwsgi ... workers=128
```

---

## 10. Remaining useful work

1. Run a reverse-order native-then-stock pair if a tighter confidence interval
   than the current parity result is required.
2. Add native support for the workload's custom middleware before expecting
   the outer-loop TE gains to appear in DCPerf.
3. Persist per-iteration raw metrics as separate result artifacts rather than
   relying only on the parser's retained middle sample.
