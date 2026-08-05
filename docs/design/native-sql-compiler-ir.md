# Native SQL Compiler: Full-Coverage IR and Dialect Design

**Status:** Draft (aggressive / full-coverage)  
**Date:** 2026-08-05  
**Branch context:** cppdjango 6.0.7  
**End-state goal:** **Every** statement Django’s ORM can produce is compiled by the **C++ SQL compiler**. The Python `SQLCompiler` is a migration scaffold, not a permanent dual engine.

---

## 0. North star

```
Python ORM API  →  resolve names / mutate Query  →  lower to IR  →  C++ compile  →  SQL + params
                                                                              ↑
                                                                    only compiler
```

**Not the goal:** a permanent “simple query fast path” beside Python forever.  
**Not the goal:** more `simple_*_sql` string helpers.  
**The goal:** replace the **compiler architecture** (`as_sql` walk, dialects, statement assembly) with a C++ engine that is **complete**.

| Layer | Stays Python (long term) | Moves to C++ (long term) |
|-------|--------------------------|---------------------------|
| Models, Fields, Meta, Managers | Yes | No |
| QuerySet / Q / filter resolution | Yes (front end) | Optional later |
| `Query` graph mutation | Yes initially | May share IR builder in C++ later |
| Expression/Lookup **meaning** as SQL | No | **Yes — IR + compile** |
| `SQLCompiler` / insert/update/delete/aggregate compilers | Temporary fallback only | **Yes — sole producer of SQL** |
| `get_db_prep_*` / converters | Yes v1–v2 | Optional later |
| `cursor.execute` / `from_db` | Yes | Optional later |

**Completeness definition:** For any `Query` / `InsertQuery` / `UpdateQuery` / `DeleteQuery` / `AggregateQuery` that stock Django 6.0.7 can compile without error on a supported backend, the native compiler produces **semantically equivalent** SQL and params (golden + execution tests). Unsupported third-party backends may remain Python; first-party **PostgreSQL, MySQL, SQLite** (and Oracle if/when we claim it) are in scope.

---

## 1. Problem statement

Stock Django compiles SQL by a Python object walk:

```python
# SQLCompiler.compile
sql, params = node.as_sql(self, self.connection)  # or as_<vendor>
```

That walk spans ~9k+ LOC across compiler, query, where, lookups, expressions, plus backend `ops`. TE “fast paths” skip it for a few shapes. That is reconnaissance, not the product.

To own compilation:

1. Represent every compilable construct in a **closed, extensible IR**.
2. Implement **lowering once in C++**.
3. Implement **dialects** for each backend’s SQL surface.
4. Make Python’s job **lower Query → IR** (and prepare params), not emit SQL.

During migration, incomplete IR coverage may fall back to Python. **Fallback is a bug queue that must go to zero** for first-party backends—not an accepted dual architecture.

---

## 2. Design principles (aggressive)

1. **One compiler.** Success path never mixes Python `node.as_sql` with C++ fragments.
2. **IR is total for the ORM surface.** If Django can express it, IR can express it (possibly via a controlled `RawFragment` only for true vendor ops / `RawSQL` / `extra()`).
3. **No permanent “miss list” as product policy.** Misses are **implementation gaps** tracked to zero.
4. **Dialects are first-class.** Feature flags and quoting live in C++; Python does not stitch vendor SQL during compile.
5. **Builder is boring; compiler is smart.** Python resolves fields and copies a finished graph into IR. C++ owns parenthesization, empty/full result short-circuit, join SQL, combinators, RETURNING, etc.
6. **Parity before cleverness.** Match stock SQL semantics; optimize (plan cache, fewer parentheses) only with tests.
7. **Kill glue APIs.** Public `simple_select_*` disappear as soon as the plan path covers them.

---

## 3. Ownership and data flow

```
┌─────────────────────────────────────────────────────────────────┐
│ PYTHON — front end                                                │
│  QuerySet, Query, WhereNode, Lookups, Expressions (as data)       │
│  resolve_expression, build_filter, join setup, annotation masks     │
│  Field.get_db_prep_value / get_db_prep_save                         │
└───────────────────────────────┬─────────────────────────────────────┘
                                │  lower_to_ir(query|*, connection)
                                │  → Plan + ParamTable
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│ C++ — sql::Compiler                                               │
│  validate(plan)                                                   │
│  compile(plan, dialect) → Compiled { sql, param_order }           │
│  optional: fingerprint cache                                      │
└───────────────────────────────┬─────────────────────────────────────┘
                                │
                                ▼
                     Python: bind params → cursor.execute
```

**Forbidden on native path:** `lookup.as_sql`, `expression.as_sql`, `WhereNode.as_sql` calling back into Python during compile.

**Allowed on native path:** Python runs **before** compile to fill IR (including evaluating things that must be Python: `resolve_expression` setup, `get_db_prep_*` into the param table).

---

## 4. IR architecture (full coverage)

### 4.1 Module layout

```
cpp/sql/
  ir.hpp / ir.cpp           # nodes, Plan, arenas
  dialect.hpp / dialect.cpp # Postgres, MySQL, SQLite, (Oracle)
  compile.hpp / compile.cpp # compile(Plan) → Compiled
  lower/                    # optional C++ helpers for complex emit
  cache.hpp                 # plan fingerprint → sql
  bind.cpp                  # nanobind: compile_plan

django/db/models/sql/
  ir_builder.py             # Query* → Plan (the lowerer)
  compiler.py               # thin: build IR → native compile → else legacy
```

### 4.2 Core IR: expressions first

Everything is an expression tree. Statements are roots. This matches Django’s model (`Expression`, `Lookup`, `WhereNode`) without keeping Python classes in the hot path.

```text
Expr =
  # Atoms
  | Column { alias: Ident, name: Ident }           # Col
  | ColumnPair { ... }                             # ColPairs / composite PK
  | Star { alias: Ident | None }
  | Param { slot: u32 }                            # bound value placeholder
  | Null
  | True | False
  | Integer { value: i64 }                         # only for compiler-made constants
  | Float { value: f64 }                           # rare; prefer Param
  | String { value: str }                          # rare; prefer Param (literals in extra)
  | Ref { name: Ident }                            # annotation / SELECT alias ref
  | OuterRef { name: Ident }                       # subquery outer ref
  | ResolvedOuterRef { ... }

  # Combinators / ops
  | BinOp { op: BinOpKind, lhs: Expr, rhs: Expr }  # CombinedExpression (+, -, ||, …)
  | UnaryOp { op: UnaryOpKind, expr: Expr }         # NegatedExpression, etc.
  | Func { name: Ident, args: [Expr],
           template: FuncTemplate | None,          # Django Func.template / arg_joiner
           extra: FuncExtra }                      # DISTINCT, order_by inside func, …
  | Cast { expr: Expr, output_db_type: TypeName }
  | ValueRef = Param | Null | …                    # resolved Value()

  # Control
  | Case { cases: [(Expr when, Expr then)],
           default: Expr | None }
  | When already folded into Case entries

  # Subquery
  | SubqueryExpr { plan: SelectPlan,            # nested plan owned by IR
                   template: SubqueryShape }    # bare / EXISTS / IN-subquery
  | Exists { plan: SelectPlan, negated: bool }

  # Window
  | Window { content: Expr, partition_by: [Expr],
             order_by: [OrderExpr], frame: Frame | None }

  # Order wrapper
  | OrderExpr { expr: Expr, desc: bool,
                nulls: NullsOrder }

  # Escape hatch (required for completeness, not for laziness)
  | RawSql { sql: str, params: [u32 slots],
             contains_aggregate: bool,
             contains_over: bool }
  # Used for: Query.extra, RawSQL expression, vendor-specific ops,
  # third-party expressions during bring-up. Every RawSql in tests
  # is a ticket to replace with structured IR when feasible.
```

`BinOpKind` covers Django `Combinable` connectors: `ADD`, `SUB`, `MUL`, `DIV`, `MOD`, `POW`, `BITAND`, `BITOR`, `XOR`, `CONCAT` (and dialect rewrites like `||` vs `CONCAT()`).

`Func` is how **annotations and database functions** lower: `Count`, `Coalesce`, `Upper`, … The builder maps `expression.__class__` + state → `Func{name, args}` or a known intrinsic. Unknown `Func` subclasses → `RawSql` from a **one-time** Python `as_sql` **only in the builder** (still not during C++ walk)—or better, register a template in a Python→IR registry.

### 4.3 Predicates and boolean trees

```text
Pred =
  | Compare { op: CmpOp, lhs: Expr, rhs: Expr }     # exact, gt, … (rhs may be Param or Expr)
  | In { lhs: Expr, values: [Expr] | SubqueryExpr }
  | InEmpty                                          # canonical empty IN → compile to false
  | IsNull { expr: Expr, negated: bool }
  | Pattern { op: PatternOp, lhs: Expr, rhs: Expr,  # contains/startswith/… 
              same: PatternMode }                   # sensitive / insensitive
  | Regex { lhs: Expr, rhs: Expr, insensitive: bool }
  | Range { lhs: Expr, start: Expr, end: Expr }
  | Builtin { name: Ident, lhs: Expr, rhs: Expr }   # extensible
  | RawPred { sql: str, params: [u32] }             # temporary

BoolExpr =
  | Atom(Pred)
  | And { children: [BoolExpr] }
  | Or  { children: [BoolExpr] }
  | Xor { children: [BoolExpr] }                    # dialect: native XOR or rewrite
  | Not { child: BoolExpr }
  | ConstTrue | ConstFalse                          # FullResultSet / EmptyResultSet
```

Builder maps every `Lookup` subclass used by Django into `Pred`. Unknown lookup → builder invokes a **registration hook** or emits `RawPred` from a single Python `as_sql` at build time (gap), logged as incomplete.

**Empty / full short-circuit:** C++ WHERE emitter implements the same semantics as `WhereNode.as_sql` (empty child counts, NOT flip, XOR rewrite to Case/Mod for backends without XOR). This logic moves **into C++**, not left in Python.

### 4.4 FROM / JOIN graph

```text
TableExpr =
  | BaseTable { table: Ident, alias: Ident }
  | Join {
      table: Ident,
      alias: Ident,
      join_type: Inner | LeftOuter | RightOuter | FullOuter,
      on: BoolExpr,                    # or join cols from Join object
      nullable: bool,
    }
  # Subquery in FROM:
  | FromSubquery { plan: SelectPlan, alias: Ident }

FromClause {
  root: TableExpr,
  joins: [Join],                       # ordered
}
```

Builder copies `query.alias_map` + join types + join fields into this graph. **Join promotion stays in Python** while `Query` lives in Python; compile only emits SQL for the final graph.

### 4.5 SELECT statement (full)

```text
SelectPlan {
  dialect: DialectId

  # WITH (CTE) — if/when Query gains or backends need; v-later
  with_clauses: [Cte]

  distinct: DistinctNone | DistinctAll | DistinctOn([Expr])
  select_list: [SelectItem]            # expr + optional out_alias
  from: FromClause | None              # bare SELECT allowed if dialect needs
  where: BoolExpr | None
  group_by: GroupByNone | GroupByExprs([Expr]) | GroupBySelectAll
  having: BoolExpr | None
  qualify: BoolExpr | None             # window filter
  order_by: [OrderExpr]
  limit: Limit | None                  # { count: u64 | None, offset: u64 }
  for_update: ForUpdate | None
  combinator: Combinator | None
}

SelectItem { expr: Expr, alias: Ident | None }

Combinator {
  op: Union | UnionAll | Intersect | IntersectAll | Except | ExceptAll
  parts: [SelectPlan]                  # nested plans
}

ForUpdate {
  no_key: bool
  nowait: bool
  skip_locked: bool
  of: [Ident]                          # table aliases / columns per dialect
}
```

This is intentionally aligned with stock `SQLCompiler.as_sql` stages: select, distinct, from, where, group by, having, qualify, order by, limit, for update, combinator, explain.

```text
Explain { format: str | None, options: map }  # wraps SelectPlan if query.explain_info
```

### 4.6 INSERT / UPDATE / DELETE / aggregate

```text
InsertPlan {
  dialect: DialectId
  table: Ident
  columns: [Ident]
  rows: [[Expr]]                       # each cell Param/Null/Expr
  on_conflict: None | Ignore | Update {
      update_fields: [Ident],
      update_exprs: [Expr],
      constraint: … | None
    }
  returning: [Ident] | [Expr]
}

UpdatePlan {
  dialect: DialectId
  table: Ident
  assignments: [(Ident, Expr)]         # Expr may be Param, BinOp(F), Func, …
  from_clause: FromClause | None       # backends that support UPDATE … FROM
  where: BoolExpr | None
  returning: [Expr]
}

DeletePlan {
  dialect: DialectId
  table: Ident
  where: BoolExpr | None
  # multi-table delete variants per dialect as needed
}

AggregatePlan {
  # SELECT aggregate over inner query — mirrors SQLAggregateCompiler
  dialect: DialectId
  select_list: [SelectItem]
  from_subquery: SelectPlan
}
```

**All** of `SQLInsertCompiler`, `SQLUpdateCompiler`, `SQLDeleteCompiler`, `SQLAggregateCompiler` lower to these roots. No parallel Python emit long term.

### 4.7 Param table

```text
ParamTable {
  # slot id → Python-side prep recipe (kept in Python)
  # IR only stores slot ids inside Param { slot }
}

Compiled {
  sql: string
  param_order: [u32]                   # left-to-right placeholder order
}
```

v1: Python fills values after compile.  
v2 (optional): typed scalars in C++ for int/bool/str/bytes/None to skip prep for simple fields.

### 4.8 RawSql policy (completeness without lying)

`RawSql` / `RawPred` / `Query.extra` exist so **coverage is total**:

| Source | IR |
|--------|-----|
| `RawSQL(...)` expression | `RawSql` |
| `Query.extra` select/where/tables | `RawSql` / raw from fragments |
| `Func` with custom `as_sql` not yet structured | Temporary: builder runs Python `as_sql` **once**, stores result as `RawSql` |
| Third-party lookups | Same temporary path + registry |

**Rule:** Raw nodes are valid IR and compile in C++ (pass-through splice + params). They are **not** an excuse to leave structured Django builtins untranslated. CI metric: count of Raw nodes on Django’s own test suite must trend down; builtins must be structured.

---

## 5. Dialects (full backend surface)

### 5.1 Responsibilities

Each `Dialect` implements everything `DatabaseOperations` + `features` imply for SQL **text**:

| Concern | Dialect API (illustrative) |
|---------|----------------------------|
| Identifiers | `quote(ident)`, `quote_alias` |
| Placeholders | `placeholder(i)` → `%s` (or native later) |
| LIMIT/OFFSET | `limit_offset(limit, offset)` incl. offset-only / no_limit_value |
| DISTINCT ON | support or error |
| Boolean | TRUE/FALSE vs 1/0 |
| XOR | native vs rewrite |
| Pattern ops | LIKE / ILIKE / UPPER(LIKE) |
| Regex | `~` / `REGEXP` / … |
| FOR UPDATE | NOWAIT, SKIP LOCKED, OF, NO KEY |
| RETURNING | support + syntax |
| INSERT ON CONFLICT | PG / MySQL / SQLite variants |
| Empty set | `0=1` vs `FALSE` |
| Explain | prefix |
| Set ops | UNION ALL … parentheses rules |
| Date/time funcs | if Func templates differ |
| Cast | `CAST` / sqlite numeric hacks |

### 5.2 Feature matrix

Builder consults `connection.features` **before** IR finalization to:

- reject or rewrite unsupported combinations the same way stock raises `NotSupportedError`, or  
- encode dialect-specific IR alternatives.

Compile-time: dialect asserts features; errors match Django messages where tests depend on them.

### 5.3 First-party targets

| Backend | Priority |
|---------|----------|
| PostgreSQL | P0 (TE + production) |
| SQLite | P0 (dev + tests) |
| MySQL/MariaDB | P0 |
| Oracle | P1 when claimed |

---

## 6. Builder: `Query` → IR (Python)

### 6.1 Entry points

```python
def lower_select(compiler: SQLCompiler) -> tuple[SelectPlan, ParamTable] | None
def lower_insert(compiler: SQLInsertCompiler) -> ...
def lower_update(compiler: SQLUpdateCompiler) -> ...
def lower_delete(compiler: SQLDeleteCompiler) -> ...
def lower_aggregate(compiler: SQLAggregateCompiler) -> ...
```

During migration, `None` means gap → legacy Python compile.  
**Target:** these never return `None` for first-party backends on Django’s own suite.

### 6.2 Expression lowering registry

```python
# Maps type(expr) → callable(expr, ctx) -> ir.Expr
EXPRESSION_LOWERING: dict[type, Callable]

def lower_expr(expr, ctx: LowerCtx) -> ir.Expr:
    for cls in type(expr).mro():
        if cls in EXPRESSION_LOWERING:
            return EXPRESSION_LOWERING[cls](expr, ctx)
    # Completeness path: one-shot Python as_sql → RawSql (gap metric++)
    return raw_from_python_as_sql(expr, ctx)
```

Same for lookups → `Pred`.

**Aggressive rule:** Every class in `django.db.models.expressions` and `lookups` and `functions.*` gets a structured lowerer. Raw path is only for unknown third parties and true raw SQL.

### 6.3 Where / join / select

- Walk `WhereNode` recursively → `BoolExpr` (And/Or/Xor/Not/Atom).  
- Map `EmptyResultSet` / `FullResultSet` to `ConstFalse` / `ConstTrue` during emit (or builder).  
- `get_select` equivalent: convert each selected expression + alias.  
- `alias_map` → `FromClause`.  
- Combinator: recursively lower each combined query to nested `SelectPlan`.

### 6.4 Integration

```python
# SQLCompiler.as_sql
def as_sql(self, ...):
    if native_sql_enabled():
        lowered = lower_select(self)
        if lowered is not None:
            plan, ptable = lowered
            sql, order = native.sql_compile(plan)
            return sql, ptable.bind(order)
    return self._as_sql_legacy(...)
```

When gap count is zero: delete `_as_sql_legacy` for first-party path; keep only behind `DJANGO_NATIVE_SQL=0` emergency valve.

---

## 7. C++ compiler responsibilities

1. **Validate** IR (arity, dialect constraints).  
2. **Emit** SQL with correct precedence/parentheses.  
3. **Flatten** params in visit order.  
4. **Short-circuit** ConstTrue/ConstFalse in boolean trees (match Django).  
5. **Combinators** with backend paren/limit rules.  
6. **Cache** compiled SQL by plan fingerprint (structure only; IN arity included).  
7. **Never** call Python.

Performance target: one nanobind crossing per statement compile (cache hit: near zero C++ work after hash lookup).

---

## 8. Migration strategy (aggressive timeline)

Fallback is allowed **only** as a temporary gap. Each phase **expands IR until legacy is dead**.

| Phase | Scope | Definition of done |
|------:|-------|--------------------|
| **0** | Design + `cpp/sql` skeleton + golden harness | This doc + empty compile returns error |
| **1** | Full `Expr`/`BoolExpr`/`SelectPlan` IR types + dialects quote/limit | C++ unit tests |
| **2** | Single-table SELECT: all builtin lookups, values/values_list, order, limit, distinct | TE `/db` queryN + large golden slice; **remove** `simple_select_*` |
| **3** | Joins + `select_related` SQL + multi-alias columns | `filter(fk__x)`, joins golden |
| **4** | Annotations: `Func`, `Aggregate`, `OrderBy`, `Ref`, GROUP BY/HAVING | `annotate`/`aggregate` suite |
| **5** | Subquery / Exists / OuterRef / combinators | `queries` tests for subqueries & union |
| **6** | Window / qualify / Frame | window tests |
| **7** | UPDATE / DELETE / INSERT / RETURNING / ON CONFLICT | write suite + TE update via ORM if used |
| **8** | `extra` / `RawSQL` / explain as Raw + structured explain | full `queries` module |
| **9** | **Gap = 0** on SQLite + Postgres Django test suite for compile | Legacy `as_sql` body unused |
| **10** | MySQL gap = 0; delete or `#ifdef` legacy compiler | Ship default native-only compile |

**Parallel work:** keep TE RPS green every phase; never block completeness on TE-only shortcuts.

### Gap accounting

```text
NATIVE_SQL_GAPS = counter of RawSql emitted by fallback lowerers
                 + counter of lower_* returning None
```

CI fails if gaps increase on main; weekly goal is monotonic decrease to zero.

---

## 9. Testing (parity is the product)

### 9.1 Golden SQL

For each backend:

- Run stock compiler (`DJANGO_NATIVE_SQL=0`) and native compile on the same `Query`.  
- Normalize whitespace/parens where semantics equal; prefer **exact match** when stock is stable.  
- Corpus = entire Django `tests/queries`, `expressions`, `aggregation`, `lookup`, `schema` touchpoints that produce SQL.

### 9.2 Execution parity

Same inputs → same rows / rowcounts / exceptions (`EmptyResultSet`, `NotSupportedError`).

### 9.3 Mutation / fuzz (aggressive)

- Random Q trees from a grammar of lookups; compare stock vs native SQL/results.  
- Optional SQLTruth or differential testing against Postgres.

### 9.4 TE

Regression gates on RPS, not the design driver.

---

## 10. Caching and COW

- Fingerprint: canonical serialization of plan **structure** (not param values).  
- IN-list length in fingerprint.  
- Intern SQL strings; process-local cache; warmup after fork.  
- Goal: multi-worker `d_priv_dirty` no worse than stock for compile caches.

---

## 11. Relationship to current dual-path / TE code

| Artifact | Fate |
|----------|------|
| `simple_select_eq_limit_sql` etc. | Delete after Phase 2 |
| QuerySet `_fast_path_simple_get` | Keep **execution** shortcuts (`fetchone`, `from_db`) if still faster; SQL only from compiler |
| `render_fortune_page` | Out of scope (HTML), unchanged |
| Nanobind helpers for clause fragments | Inline into dialect emit; stop exporting as ORM API |
| `DJANGO_NATIVE=0` | Disables all native including SQL compiler |

---

## 12. Risks (accepted)

| Risk | Mitigation |
|------|------------|
| Huge surface | Registry + gap metrics + phase gates; don’t stop at TE subset |
| Semantic drift | Golden tests vs stock as oracle |
| Vendor quirks | Dialect matrix + backend CI |
| Third-party expressions | RawSql bridge + documented extension registry |
| Build time / binary size | Separate `cpp/sql` translation unit; still one module |
| Long calendar time | Aggressive parallelization: Expr IR and joins can proceed in parallel after Phase 1 |

---

## 13. Open decisions (lean aggressive)

| Topic | Decision |
|-------|----------|
| End state dual compilers? | **No.** One compiler (C++). Legacy only as escape hatch flag. |
| All queries? | **Yes** for PG/MySQL/SQLite. |
| RawSql forever? | Only for true raw/extra/third-party; builtins structured. |
| Move Query building to C++? | **Not required** for this project; compile is the goal. Revisit after gap=0. |
| Param prep in C++? | Optional v2; not blocking completeness. |
| Oracle | P1; same IR. |

---

## 14. Success metrics

1. **Coverage:** `lower_*` never returns None on Django’s suite for PG+SQLite+MySQL.  
2. **Ownership:** No production SQL text assembled in Python for ORM queries.  
3. **Architecture:** Single `sql_compile(Plan)` entry; no public simple-SQL helpers.  
4. **Parity:** Golden + execution tests green.  
5. **Performance:** TE and general ORM no slower than pre-migration best; multi-statement workloads improve as Python `as_sql` vanishes.  
6. **Policy:** `DJANGO_NATIVE=0` still pure Python dual-path for the whole fork.

---

## 15. Immediate next steps

1. Land this doc as the working plan.  
2. Scaffold `cpp/sql/{ir,dialect,compile}.hpp` with **full** IR node enums (even if emit stubs).  
3. Implement `sql_compile` for Select + Compare/And/Param/Column/Limit (first real path).  
4. Implement `ir_builder.lower_expr` / `lower_where` / `lower_select` with **registry**, not ad-hoc TE ifs.  
5. Wire `SQLCompiler.as_sql`; turn on golden tests; ban new `simple_*` helpers.  
6. Expand registry class-by-class until gap=0—**default priority is coverage, not another TE tweak.**

---

## 16. Summary

We are building a **complete C++ SQL compiler** for Django’s ORM:

- **Total IR** for expressions, predicates, joins, select/insert/update/delete/aggregate, combinators, windows.  
- **Dialects** implement backend SQL fully.  
- **Python lowers Query → IR** and prepares params; **C++ alone emits SQL**.  
- Migration uses temporary gaps/RawSql, measured to zero—not a permanent “subset + Python” product.

The TE fast paths proved the bottleneck. This design **replaces the compiler**, not the handful of queries TechEmpower exercises.

---

## Appendix A — TE `/db` as a full-IR instance (not a special case)

```text
SelectPlan {
  select_list: [Column(world,id) AS id, Column(world,randomnumber) AS randomnumber]
  from: BaseTable(world)
  where: Compare(Eq, Column(world,id), Param(0))
  limit: { count: 21, offset: 0 }
}
```

Same machinery as `annotate(Count('x')).filter(...)`—only a larger tree.

---

## Appendix B — Expression registry (initial mandatory set)

Must have structured lowerers (no Raw) before claiming Phase 4 done:

`Col`, `ColPairs`, `F` (resolved), `Value`, `Func` (and all `django.db.models.functions`), `CombinedExpression`, `ExpressionWrapper`, `NegatedExpression`, `Case`/`When`, `Subquery`, `Exists`, `OrderBy`, `Ref`, `OuterRef`, `ResolvedOuterRef`, `Star`, `RawSQL`, `Window` + frames, `ExpressionList`, `Cast`, aggregates (`Count`, `Sum`, …).

Lookups: all of `lookups.py` builtins (`Exact`, `In`, `IsNull`, pattern, range, regex, year, uuid, integer overflow variants).

---

## Appendix C — Glossary

| Term | Meaning |
|------|---------|
| IR | Complete intermediate representation for ORM SQL |
| Builder / lowerer | Python Query → IR |
| Gap | Miss or Raw fallback still needed |
| Dialect | C++ backend SQL rules |
| Legacy compiler | Stock Python `as_sql` walk — temporary |
| Completeness | Gap=0 for first-party backends on Django tests |
