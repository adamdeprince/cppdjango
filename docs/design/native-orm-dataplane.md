# Native ORM Data Plane (Thought Experiment)

**Status:** Thought experiment / north-star architecture  
**Date:** 2026-08-05  
**Product constraint:** API-compatible with Django 6.x ORM usage  
**Performance constraint:** Hot paths are **pure C++**; Python↔C++ crossings are **not** on the per-row or per-clause critical path  

This supersedes the conservative “compiler only, QuerySet stays Python” cut. It is consistent with full-coverage SQL IR ([`native-sql-compiler-ir.md`](./native-sql-compiler-ir.md)) and pushes further: **the query engine itself lives in C++.**

---

## 1. Thesis

Leaving the QuerySet/Query graph in Python, or inserting a **C++/Python boundary per filter clause, per expression node, or per row**, steals the performance you are trying to buy.

**Goal:** API-compatible Django ORM whose **data plane** is implemented in as pure C++ as possible:

```text
App code (Python)
    │  few crossings per operation (not per node / per row)
    ▼
┌─────────────────────────────────────────────┐
│  C++ ORM data plane                         │
│  schema • queryset • query graph • compile  │
│  execute protocol • row decode (optional)   │
└─────────────────────────────────────────────┘
    │  one DB round-trip
    ▼
  Database
```

Python remains the **control plane** for what must be dynamic or ecosystem-facing (model *definitions*, app registry bootstrap, rare extension hooks). Steady-state query build → compile → fetch for normal apps should not re-enter Python until results are materialized as Python objects (and even that can be delayed/batched).

---

## 2. Compatibility vs implementation purity

| Requirement | Meaning |
|-------------|---------|
| **API compatible** | Existing apps write `Model.objects.filter(...).values_list("id").get()` and get the same results/errors. Managers, Q, F, lazy evaluation, slicing semantics preserved at the Python surface. |
| **Implementation pure C++** | After setup, the **authoritative** state of a queryset and its SQL compilation live in C++. Method bodies are C++ (exposed via nanobind), not Python algorithms that call tiny C++ helpers. |
| **Not required** | Bit-identical Python object graphs; CPython `QuerySet` source lines; every private `_foo` attribute existing forever. |
| **Allowed** | Thin Python subclasses/facades that hold a single C++ handle and forward methods. |

Dual-path policy for the fork can remain:

- `DJANGO_NATIVE=0` → stock pure-Python ORM  
- default → native data plane  

That is a **product** dual-path, not a hot-path dual implementation glued together.

---

## 3. The real enemy: boundary tax

### 3.1 Cost model

Every Python↔C++ crossing roughly pays:

- argument conversion (nanobind)  
- call overhead  
- possible allocation of temporary Python objects  

If a request does:

```text
20 × ( filter + compile_helpers + fetchrow + wrap )
```

with **dozens of crossings each**, native “acceleration” loses to stock on small work (same failure mode as json/plaintext framework tax).

### 3.2 Rules for the thought experiment

1. **Batch the boundary.** Prefer one crossing: `qs.execute_values_list()` → list of tuples, not 12 crossings to build SQL and 20 to wrap rows.  
2. **No per-node compile callbacks into Python.** Expression trees are C++ values.  
3. **No per-lookup `as_sql` in Python.**  
4. **Schema is snapshotted into C++** so `filter(id=7)` does not call `model._meta.get_field` on every chain step.  
5. **Materialization is the main intentional re-entry** into Python (building model instances), and should be specialized (e.g. values_list stays in C++/compact form longer).

### 3.3 Hot path definition (TE-shaped and general)

Hot:

- `filter` / `exclude` / `order_by` / `values` / `values_list` / `annotate` (common)  
- `get` / `first` / `count` / `exists`  
- `update` / simple `save`  
- SQL compile + bind + fetch  
- decoding rows to scalars / tuples / dicts  

Cold (Python OK):

- first import of models, `AppConfig.ready`  
- migrations, schema editor  
- admin, forms, serializers (unless later ported)  
- defining new `Field` subclasses  
- rare `QuerySet` subclass hooks  

---

## 4. Target architecture

```text
                         PYTHON CONTROL PLANE
┌──────────────────────────────────────────────────────────────────┐
│  class World(Model): ...          # defines schema in Python     │
│  apps.populate() → register_model → **export schema to C++**     │
│  Manager descriptor → returns QuerySet **facade** (handle only)  │
│  Q, F: either facades over C++ expr nodes or constructors that    │
│         immediately create C++ nodes                              │
└───────────────────────────────┬──────────────────────────────────┘
                                │ nanobind: few entry points
                                ▼
                          C++ DATA PLANE
┌──────────────────────────────────────────────────────────────────┐
│  SchemaRegistry                                                  │
│    ModelId → Table, FieldId→Column, joins, lookup map, pk        │
│                                                                  │
│  orm::QuerySet                                                   │
│    holds orm::Query (value or shared_ptr, COW clone on write)    │
│    filter/exclude/annotate/values/... mutate or return new QS    │
│                                                                  │
│  orm::Query                                                      │
│    aliases, joins, where, select, annotations, order, slice      │
│    = the IR / isomorphic to full SQL IR                          │
│                                                                  │
│  sql::Compiler + Dialects                                        │
│    Query → SQL string + param arena                              │
│                                                                  │
│  exec:: (optional phase)                                         │
│    bind params, run statement, decode rows to native buffers     │
└───────────────────────────────┬──────────────────────────────────┘
                                │ one execute
                                ▼
                         libpq / DB-API bridge
```

**Key idea:** `Query` **is** the IR. You do not build a Python `Query`, then translate to IR, then compile. Translation is a permanent boundary tax.

---

## 5. Layer breakdown

### 5.1 Schema export (setup, not hot path)

At model class_prepare / apps ready:

```text
Python Meta  →  C++ SchemaRegistry::upsert(ModelSchema)
```

`ModelSchema` includes:

- `db_table`, app label, model name  
- fields: attname, column, type tag, null, primary_key, unique, db_index  
- relations: remote model id, join columns, M2M through tables  
- concrete field order for `from_db`  
- default ordering  

Lookups: builtin map `exact/in/gt/...` → enum. Custom lookups registered via a Python hook that installs a C++ vtable or a “raw lowerer” id (cold).

**Hot `filter(pk=1)`** uses only `SchemaRegistry`, not `Options.get_field`.

### 5.2 QuerySet (C++)

```cpp
class QuerySet {
  SchemaRegistry* db;
  ModelId model;
  Query query;              // the graph
  // flags: result type values/list/model, deferred, prefetch specs, ...
public:
  QuerySet filter(const PredBuilder&);   // or kwargs from Python once
  QuerySet exclude(...);
  QuerySet order_by(...);
  QuerySet values(...);
  QuerySet values_list(..., bool flat);
  QuerySet annotate(...);
  QuerySet select_related(...);
  QuerySet chain_combinator(...);
  // terminals:
  Row get();
  std::vector<Row> fetch_all();
  int64_t count();
  bool exists();
  int64_t update(const Assignments&);
  // ...
};
```

Clone semantics: cheap COW (`shared_ptr<QueryData>` + unique on write) so chains do not deep-copy huge trees.

### 5.3 Python facade (API compatibility)

```python
class QuerySet:  # django.db.models.query.QuerySet
    def __init__(self, model=None, query=None, using=None, hints=None, _native_handle=None):
        self._n = _native_handle or native.orm.QuerySet(model_id, using)

    def filter(self, *args, **kwargs):
        return QuerySet(_native_handle=self._n.filter(args, kwargs))

    def get(self, *args, **kwargs):
        return materialize(self._n.get(args, kwargs), self.model)
```

Rules:

- Facade methods are **one-liners** into C++.  
- No Python reimplementation of where-building.  
- `Q` objects: either wrap `native.orm.BoolExpr` or lower to C++ in `Q.__init__` / `QuerySet.filter(Q)`.  
- Subclasses of QuerySet: supported if they call `super().filter` / return facades that keep the same handle type; deep overrides of `_fetch_all` need explicit native hooks.

### 5.4 kwargs bridging (necessary evil, minimize cost)

`filter(id=7, name__in=["a","b"])` must enter C++ **once**:

```text
filter_kwargs(qs_handle, py_dict) → new qs_handle
```

Inside C++ (or a single tight bridge):

- parse key on `__`  
- resolve path against SchemaRegistry  
- attach joins  
- build predicates  
- store param values in a **C++ param arena** (typed: i64, str, bool, none, bytes, list…)

Avoid:

```text
for each kwarg:
    python_resolve_field()
    python_build_lookup()
    cpp_add_node()   # N crossings
```

### 5.5 Compiler (C++)

As in the full-coverage SQL design: **complete** dialected emit for all plans the ORM can build. No Python `as_sql` walk on the native path.

### 5.6 Execution and materialization

**Phase A (minimum pure):** C++ builds SQL + param list → Python `cursor.execute` → Python fetch → materialize.  
Still **one** crossing for execute; compile is pure C++. Param list conversion is one crossing.

**Phase B (stronger):** C++ talks to libpq / DB-API driver adapter; decodes into `Arena` of rows (columns as variants).  
Python materialization pulls from arena:

- `values_list` → `list[tuple]` built in C++ and handed over once  
- model instances → `from_db` bulk path reading arena  

**Phase C (maximum thought experiment):** optional “native result” consumed by a native HTTP layer; out of scope for API-compatible Django but shows the asymptote.

For API-compatible Django, **Phase B** is the practical purity target: Python sees results, not intermediate query graphs.

---

## 6. What stays in Python (and why)

| Piece | Why Python |
|-------|------------|
| `class Model` body, Field instances | Metaclass ecosystem, descriptors, migration deconstruct |
| App registry bootstrap | Import side effects, `AppConfig` |
| Migrations | Historical projects, RunPython |
| Admin / forms / DRF | Separate stacks; can call ORM facade |
| Custom `Field.from_db_value` | User Python callables (invoke only at materialize) |
| Signals `pre_save` / `post_save` | User receivers; fire around native save |
| Async ORM | Can wrap native sync in threadpool or later native async |

**Important:** “Field defined in Python” does **not** mean “field resolution on every filter in Python.” Export once to C++ schema.

---

## 7. Completeness

Same north star as the SQL compiler doc: **all** ORM queries that stock Django can run on PG/MySQL/SQLite work on the native data plane.

Implementation strategy:

- Full C++ expression/predicate/join/statement model  
- Schema + registry for builtins  
- Extension points for third-party lookups/fields (cold registration)  
- Gap metrics during port; **zero gaps** is the release bar for “native default”  

No permanent “simple queryset only” product tier.

---

## 8. Performance thought experiment (what we expect)

### 8.1 Where pure C++ wins

| Workload | Why |
|----------|-----|
| Many `filter`/`exclude` chains | No Python object graph churn |
| `get` / `values_list.get` loops (queryN) | Build+compile in C++; optional stmt cache |
| Complex Q trees | Boolean tree in C++ |
| annotate + values | Expression IR compile once |
| Large `values()` fetches | Decode in C++; one handoff of list |
| Multi-worker COW | Shared schema; COW query trees; interned SQL |

### 8.2 Where boundaries still exist

| Crossing | Frequency target |
|----------|------------------|
| `Model.objects` → native QS | 1 per chain start |
| `.filter(**kwargs)` | 1 per call (kwargs blob in) |
| `.get()` / `.all()` materialize | 1 out with all rows |
| First schema export | once per model per process |
| Signals / custom from_db_value | per row only if user defined |

### 8.3 What we refuse

- C++ formats `"%s"` while Python walks `WhereNode` children  
- Nanobind per SQL clause  
- Dual graphs (Python Query + C++ IR) in steady state  
- TE-only assemblers as the architecture  

---

## 9. API surface mapping (illustrative)

| Python API | C++ ownership |
|------------|---------------|
| `QuerySet.filter/exclude` | `QuerySet::filter` + kwargs bridge |
| `Q` / `~Q` / `&` / `\|` | `BoolExpr` operators |
| `F`, `Value`, `Func`, `Case` | `Expr` nodes |
| `annotate`, `alias` | annotation map on `Query` |
| `values`, `values_list`, `dates` | select list + result mode |
| `order_by`, `distinct`, slicing | query clauses |
| `select_related`, `prefetch_related` | join plan + prefetch plan (prefetch may stay hybrid longer) |
| `union` / `intersection` / `difference` | combinator queries |
| `get`, `first`, `last`, `earliest`, `latest` | compile + execute + decode |
| `count`, `exists`, `aggregate` | specialized plans |
| `create`, `update`, `delete`, `bulk_*` | DML plans |
| `in_bulk`, `iterator` | execute modes |
| `select_for_update` | clause on Query |
| Manager / `RelatedManager` | facade → native QS with prefiltered relation |

`prefetch_related` is the hardest (Python objects graph, caches on instances). Thought experiment still places **plan construction** in C++; running secondary queries and attaching caches may call into Python for instance identity until a native instance cache exists.

---

## 10. Phased path (from here to the thought experiment)

These phases are intentional; earlier dual-path TE work is scaffolding only.

| Phase | Outcome | Boundary profile |
|------:|---------|------------------|
| **0** | Agree north star (this doc) | — |
| **1** | Full SQL IR + C++ compiler; Python Query still exists; single lower `Query→IR` | 1 lower + 1 compile (still dual graph) |
| **2** | C++ `Query` authoritative; Python QuerySet facade; lowerer deleted | filter/compile in C++; execute via Python cursor |
| **3** | SchemaRegistry export; kwargs filter entirely in C++ | no meta on hot filter |
| **4** | DML + full queryset terminal methods | — |
| **5** | Native decode arena + bulk materialize | 1 crossing out for results |
| **6** | Optional libpq path; stmt cache | minimum Python on TE |
| **7** | Gap=0 vs Django suite; legacy ORM only behind `DJANGO_NATIVE=0` | product dual-path |

**Do not** invest in new Python-side SQL string fast paths except as temporary scaffolding deleted by Phase 2.

---

## 11. Risks and non-goals

### Risks

| Risk | Mitigation |
|------|------------|
| nanobind facade still slow if chatty | Design terminals (`get`, `fetch_values`) not micro-ops |
| QuerySet subclassing | Document supported patterns; hooks for `_clone` |
| Third-party fields/lookups | Registration API into C++ |
| Behavioral drift | Stock oracle tests, golden SQL, differential fuzz |
| Engineering years | Completeness metrics; don’t stop at TE |

### Non-goals (for this experiment)

- Rewrite templates/HTTP in C++ (separate)  
- Bit-identical traceback internals  
- Supporting every private underscore API  
- Perfect Oracle day one  

---

## 12. Dual-path policy (fork)

```text
DJANGO_NATIVE=0  → pure Python Django ORM (correctness / escape hatch)
DJANGO_NATIVE=1  → C++ data plane default (this design)
```

No steady-state “half Python QuerySet + half C++ helpers” on the hot path under `DJANGO_NATIVE=1`.

---

## 13. Success criteria (thought experiment)

1. **Purity:** For `values_list().get()` / `filter().count()` / `filter().update()`, zero Python involvement between facade entry and SQL execute prep—no Python where-tree, no Python compiler.  
2. **Compatibility:** Django ORM test suite passes on PG/SQLite/MySQL with native default.  
3. **Boundaries:** Crossings per TE `/db` request counted and minimized (target: O(1) in, O(1) out).  
4. **Performance:** Dominate stock on ORM endpoints; do not lose on json/plaintext due to accidental facades.  
5. **Architecture:** One query graph, one compiler, schema snapshots—not assemblers.

---

## 14. Relation to prior docs / code

| Artifact | Role under this north star |
|----------|----------------------------|
| [`native-sql-compiler-ir.md`](./native-sql-compiler-ir.md) | Compiler + IR completeness — **embeds into** C++ `Query` |
| TE `simple_*_sql` helpers | Disposable scaffolding |
| QuerySet `_fast_path_*` in Python | Disposable once Phase 2 lands |
| `render_fortune_page` | Separate (HTML), not ORM data plane |

---

## 15. Bottom line

**Yes — the QuerySet API should be implemented in C++** for this thought experiment, behind an API-compatible Python facade.

The performance story is not “call C++ from Python a lot.” It is:

> **Enter C++ once, build and compile the query entirely in C++, touch the database, return results in as few crossings as possible.**

Python defines models and hosts the app.  
**C++ owns the ORM data plane end-to-end.**

That is the purest form of what you asked for: API compatible, implementation as pure C++ as the ecosystem boundary allows, without constant C++/Python chatter on the hot path.
