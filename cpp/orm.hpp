// ORM / SQL foothold helpers (WhereNode, quote_name, lookup fragments).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace django::native {

// quote_name for backends using "..." (sqlite/postgres) or `...` (mysql).
// Already-quoted names (matching outer quotes) are returned unchanged.
// style: "double" or "backtick".
[[nodiscard]] std::string sql_quote_name(std::string_view name,
                                         std::string_view style);

// WhereNode as_sql initial counters.
// AND → (full_needed=n, empty_needed=1); else → (1, n).
[[nodiscard]] std::pair<int, int> where_needed_counts(std::string_view connector,
                                                      int n_children);

// Join SQL child fragments with connector; apply NOT / parentheses.
// parts already compiled (non-empty SQL strings).
[[nodiscard]] std::string where_combine_sql(std::string_view connector,
                                            const std::vector<std::string>& parts,
                                            bool negated, bool resolved);

// RHS for IN lookups: "%s, %s, ..." (n placeholders). Empty → "".
[[nodiscard]] std::string sql_in_placeholders(int n);

// IS NULL / IS NOT NULL (lookup_name isnull, negated flag).
[[nodiscard]] std::string sql_isnull_sql(bool negated);

// Exact/simple comparison operator fragments used by lookups.
// Returns e.g. " = %s", " != %s", " > %s", " < %s", " >= %s", " <= %s"
// or empty if unknown.
[[nodiscard]] std::string sql_comparison_rhs(std::string_view lookup_name);

// Forms: empty-value check for common EMPTY_VALUES (None not representable;
// treat empty string and empty list marker).
[[nodiscard]] bool is_form_empty_string(std::string_view value) noexcept;

// Field.has_changed after both sides are stringified/empty-normalized.
// None represented as empty string already by caller.
[[nodiscard]] bool field_str_has_changed(std::string_view initial,
                                         std::string_view data) noexcept;

// BooleanField.to_python for string inputs (and key presence for checkbox).
// Returns 1=True, 0=False. Non-string handled in Python.
[[nodiscard]] int boolean_field_to_python(std::string_view value) noexcept;

// NullBooleanField.to_python: 1=True, 0=False, -1=None.
[[nodiscard]] int null_boolean_to_python(std::string_view value) noexcept;

// ChoiceWidget.format_value core: stringify each non-null value.
[[nodiscard]] std::vector<std::string> choice_format_values(
    const std::vector<std::string>& values, bool allow_none_as_empty);

// ResponseHeaders helpers.
// Returns false if key/value has CR/LF or key non-ASCII.
[[nodiscard]] bool header_key_valid(std::string_view key) noexcept;
[[nodiscard]] bool header_value_no_newlines(std::string_view value) noexcept;

// Extract charset from Content-Type; empty if missing.
[[nodiscard]] std::string charset_from_content_type(std::string_view content_type);

// CommonMiddleware path helpers.
[[nodiscard]] bool path_ends_with_slash(std::string_view path) noexcept;
// Insert '/' before '?' or '#' if path has no trailing slash before query.
[[nodiscard]] std::string force_append_slash_path(std::string_view full_path);

// Serialize headers as HTTP bytes lines without final trailing CRLF pair join:
// returns list of "Key: Value" strings (latin-1/ascii already validated).
[[nodiscard]] std::vector<std::string> serialize_header_lines(
    const std::vector<std::pair<std::string, std::string>>& headers);

// stringformat for simple specs: s, d, i, u, f, F, g, G, e, E, x, X, o, c
// value as string; for numeric specs tries parse. nullopt → Python fallback.
[[nodiscard]] std::optional<std::string> stringformat_simple(std::string_view value,
                                                             std::string_view spec);

// Improved floatformat: p can be negative (drop trailing zeros when whole).
// use_grouping false; decimal_sep "." thousand ignored. nullopt → Python.
[[nodiscard]] std::optional<std::string> floatformat_ascii(std::string_view decimal_str,
                                                           int p);

// --- ORM depth / forms / sessions / cookies (workstreams 1-5) ---------------

// Join already-quoted identifiers with '.': table.col or just col.
[[nodiscard]] std::string sql_join_dotted(const std::vector<std::string>& parts);

// Pattern lookup param wrap: contains → %value%, startswith → value%, endswith → %value
// kind: "contains" | "startswith" | "endswith"
[[nodiscard]] std::string sql_pattern_wrap(std::string_view value, std::string_view kind);

// ChoiceField.valid_value: flat list of choice keys (stringified); optgroups flattened.
[[nodiscard]] bool choice_valid_value(std::string_view text_value,
                                      const std::vector<std::string>& choice_keys);

// Decimal string structural check (optional sign, digits, optional . digits).
[[nodiscard]] bool is_decimal_string(std::string_view value) noexcept;

// Float parse for form fields; nullopt if invalid.
[[nodiscard]] std::optional<double> form_float_to_python(std::string_view value);

// Session key: truthy and length >= min_length (Django SessionBase._validate_session_key).
// If check_charset, also require [a-z0-9] only (generated keys).
[[nodiscard]] bool is_valid_session_key(std::string_view key, int min_length = 8,
                                        bool check_charset = false) noexcept;

// Cookie samesite: "lax" | "none" | "strict" (case-insensitive). Empty → valid (unset).
[[nodiscard]] bool is_valid_samesite(std::string_view value) noexcept;

// delete_cookie secure flag: true if key starts with __Secure-/__Host- or samesite none.
[[nodiscard]] bool cookie_delete_secure(std::string_view key,
                                        std::string_view samesite) noexcept;

// Cookie max-age from timedelta-like total seconds (clamp ≥ 0).
[[nodiscard]] int cookie_max_age_seconds(double total_seconds) noexcept;

// Signing: split "payload:timestamp:sig" or "payload:sig" on sep; empty if bad.
// Returns vector of parts (2 or 3).
[[nodiscard]] std::vector<std::string> signing_split(std::string_view signed_value,
                                                     std::string_view sep);

// Compress-marker for signing.dumps: true if starts with '.'
[[nodiscard]] bool signing_is_compressed(std::string_view b64_payload) noexcept;

// Hardening: WHERE empty-parts short-circuit result codes for one child outcome.
// child_kind: 0=ok_sql, 1=empty_result, 2=full_result, 3=empty_sql
// Returns: 0=continue, 1=raise EmptyResultSet, 2=raise FullResultSet
// after updating *full_needed and *empty_needed.
[[nodiscard]] int where_child_outcome(int child_kind, bool negated, int& full_needed,
                                      int& empty_needed);

// --- Query / SQLCompiler depth (#1) -----------------------------------------

// Comma-join SQL fragments (SELECT list, GROUP BY, ORDER BY items).
[[nodiscard]] std::string sql_comma_join(const std::vector<std::string>& parts);

// "ORDER BY a, b" / "GROUP BY a, b" / "WHERE ..." helpers.
[[nodiscard]] std::string sql_order_by_clause(const std::vector<std::string>& parts);
[[nodiscard]] std::string sql_group_by_clause(const std::vector<std::string>& parts);

// expr AS "alias" when alias non-empty; else expr.
[[nodiscard]] std::string sql_expr_as(std::string_view expr_sql,
                                      std::string_view quoted_alias);

// LIMIT/OFFSET clause. limit nullopt = no LIMIT (unless offset-only needs
// no_limit_value from backend — pass that as limit when only offset).
// Empty string if both absent/zero appropriately.
[[nodiscard]] std::string sql_limit_offset_clause(std::optional<int> limit, int offset);

// JoinPromoter: effective connector under negation (NOT (a AND b) ~ OR).
[[nodiscard]] std::string join_promoter_effective_connector(
    std::string_view connector, bool negated);

// Join promotion votes: promote (outer) and/or demote (inner).
[[nodiscard]] bool join_promoter_should_promote(std::string_view effective_connector,
                                                int votes, int num_children) noexcept;
[[nodiscard]] bool join_promoter_should_demote(std::string_view effective_connector,
                                               int votes, int num_children) noexcept;

// quote_name_unless_alias: true → return name unquoted (is alias-like).
[[nodiscard]] bool quote_name_is_alias(bool in_alias_map_not_table,
                                       bool in_extra_select,
                                       bool external_alias_not_table) noexcept;

// Q helpers
[[nodiscard]] bool q_is_empty(int n_children) noexcept;

// Split lookup path on "__" (LOOKUP_SEP).
[[nodiscard]] std::vector<std::string> split_lookup_path(std::string_view path);

// First component of a lookup path (before first "__"), or whole path.
[[nodiscard]] std::string lookup_path_head(std::string_view path);

// Combine two Qs' emptiness for _combine short-circuit: 0=neither empty,
// 1=self empty (return other), 2=other empty (return self).
[[nodiscard]] int q_combine_empty_flags(bool self_empty, bool other_empty) noexcept;

// --- build_filter / lookup path resolution (#1) -----------------------------

// Join path parts with "__" (LOOKUP_SEP).
[[nodiscard]] std::string join_lookup_path(const std::vector<std::string>& parts);

// field_parts = lookup_splitted[0 : len(splitted) - n_lookup_parts]
[[nodiscard]] std::vector<std::string> lookup_field_parts(
    const std::vector<std::string>& lookup_splitted, int n_lookup_parts);

// Default lookup list when empty: ["exact"].
[[nodiscard]] std::vector<std::string> lookup_or_exact(
    const std::vector<std::string>& lookups);

// refs_expression: among annotation keys, find shortest prefix of path parts
// that matches a key. Returns {matched_key, remaining_parts}; empty matched
// means no match.
struct RefsExpressionResult {
  std::string annotation;  // empty if none
  std::vector<std::string> remaining;
};
[[nodiscard]] RefsExpressionResult refs_expression_match(
    const std::vector<std::string>& lookup_parts,
    const std::vector<std::string>& annotation_keys);

// Numbered table alias: prefix + (alias_map_size + 1)  e.g. T2
[[nodiscard]] std::string next_numbered_alias(std::string_view prefix,
                                              int alias_map_size);

// Alias refcount: new value after += amount (floor at 0 if clamp).
[[nodiscard]] int alias_refcount_add(int current, int amount,
                                     bool clamp_non_negative = false) noexcept;

// Keys whose post-count is greater than pre-count (used joins).
// pre/post are parallel lists of (alias, count); missing pre treated as 0.
[[nodiscard]] std::vector<std::string> alias_refcount_increased(
    const std::vector<std::pair<std::string, int>>& pre,
    const std::vector<std::pair<std::string, int>>& post);

// Invalid multi-lookup without field parts: len(lookup_parts) > 1 and field empty.
[[nodiscard]] bool lookup_invalid_without_field(int n_lookup_parts,
                                                int n_field_parts) noexcept;

// build_filter used_joins: aliases with refcount increase.
// (alias_refcount_increased covers this.)

// Split order_by item into (field, descending). "-foo" → ("foo", true).
struct OrderBySplit {
  std::string field;
  bool descending = false;
};
[[nodiscard]] OrderBySplit split_order_by_item(std::string_view item);

// --- QuerySet / sessions / forms / compiler leftovers (1-4) -----------------

// values_list flat validation: flat=True requires 0 or 1 field name.
// Returns 0=ok, 1=flat+named conflict, 2=flat with >1 field.
[[nodiscard]] int values_list_flags(bool flat, bool named, int n_fields) noexcept;

// Unique expression alias: base + counter while collision with existing keys.
[[nodiscard]] std::string unique_field_alias(
    std::string_view base, int start_counter,
    const std::vector<std::string>& existing_keys);

// Result-cache emptiness for exists/bool when cache is present.
[[nodiscard]] bool result_cache_truthy(bool has_cache, bool cache_nonempty) noexcept;

// Session cache key: prefix + session_key.
[[nodiscard]] std::string session_cache_key(std::string_view prefix,
                                            std::string_view session_key);

// Expiry age when `_session_expiry` is None/0 → cookie_age; when int remaining
// seconds, return that value; when treated as absolute unix seconds vs
// modification, return max(0, expiry - modification).
// mode: 0 = none/0 → cookie_age; 1 = int remaining (return expiry as-is);
//       2 = absolute unix-style (expiry - modification).
[[nodiscard]] int session_expiry_age_seconds(int cookie_age,
                                             std::optional<int> modification_age,
                                             std::optional<int> expiry) noexcept;

// timedelta → seconds: days * 86400 + seconds (Django get_expiry_age datetime path).
[[nodiscard]] std::int64_t session_delta_seconds(std::int64_t days,
                                                 std::int64_t seconds) noexcept;

// True if session_key is missing/empty (exists() short-circuit → False).
[[nodiscard]] bool session_key_missing(std::string_view session_key) noexcept;

// FOR UPDATE SQL fragment.
[[nodiscard]] std::string sql_for_update(bool no_key, bool nowait, bool skip_locked,
                                         const std::vector<std::string>& of);

// Combinator keyword: "UNION" / "UNION ALL" / "INTERSECT" / "EXCEPT" etc.
// combinator is lowercase union/intersection/difference; all only for union.
[[nodiscard]] std::string sql_combinator_keyword(std::string_view combinator,
                                                 bool all);

// Wrap each part in braces ("{}" or "({})") and join with combinator.
[[nodiscard]] std::string sql_combinator_join(std::string_view combinator_sql,
                                              const std::vector<std::string>& parts,
                                              bool wrap_parens);

// DISTINCT clause: plain "DISTINCT" when no fields; empty means caller raises.
// Returns "DISTINCT" or "DISTINCT ON (...)" when fields non-empty (postgres style).
[[nodiscard]] std::string sql_distinct_clause(const std::vector<std::string>& fields,
                                              bool allow_on);

// MultipleChoiceField.has_changed for stringified sets of equal length.
[[nodiscard]] bool multi_choice_has_changed(
    const std::vector<std::string>& initial,
    const std::vector<std::string>& data) noexcept;

// JSON structural precheck: true if looks like object/array/string/number/bool/null
// start; false → invalid. Does not full-parse (json.loads remains oracle).
[[nodiscard]] bool json_looks_valid(std::string_view value) noexcept;

// MultipleChoice to_python: stringify list (identity for already-strings).
[[nodiscard]] std::vector<std::string> stringify_choice_list(
    const std::vector<std::string>& values);

// FROM join: "a INNER JOIN b ON ..." style is complex; simple table list join.
[[nodiscard]] std::string sql_from_tables(const std::vector<std::string>& clauses);

// --- QuerySet surface (#1) --------------------------------------------------

// count() when cache is present: return cache_len (always >= 0).
// When cache absent, return -1 so Python hits the DB path.
[[nodiscard]] int queryset_count_from_cache(bool has_cache, int cache_len) noexcept;

// exists() when cache is present.
[[nodiscard]] bool queryset_exists_from_cache(bool has_cache,
                                              bool cache_nonempty) noexcept;

// first/last cache short-circuit eligibility: ordered + has_cache.
// index_mode: 0=first, 1=last. Returns true if Python should use cache index.
[[nodiscard]] bool queryset_use_cache_for_first_last(bool has_cache, bool ordered,
                                                     bool cache_nonempty) noexcept;

// iterator() chunk_size validation.
// chunk_size_none: True if caller passed None.
// Returns 0=ok, 1=need chunk_size with prefetch, 2=chunk_size <= 0.
[[nodiscard]] int iterator_chunk_validate(bool chunk_size_none, int chunk_size,
                                          bool has_prefetch) noexcept;

// Effective chunk size: None → default (2000); else positive value unchanged.
[[nodiscard]] int iterator_chunk_size_or_default(bool chunk_size_none, int chunk_size,
                                                 int default_size = 2000) noexcept;

// in_bulk: empty id_list → empty result.
[[nodiscard]] bool in_bulk_empty(bool id_list_is_none, int id_list_len) noexcept;

// field_name → "{field_name}__in"
[[nodiscard]] std::string in_bulk_filter_key(std::string_view field_name);

// Batch ranges for in_bulk: [(start, end), ...] for range(0, n, batch_size).
// batch_size <= 0 or >= n → single batch (0, n).
struct BatchRange {
  int start = 0;
  int end = 0;
};
[[nodiscard]] std::vector<BatchRange> in_bulk_batch_ranges(int n_ids, int batch_size);

// MultipleObjectsReturned / get() result classification:
// num_results, limit (0 = no limit). Returns 0=ok one, 1=none, 2=multiple.
[[nodiscard]] int get_result_kind(int num_results, int limit) noexcept;

// --- bulk SQL / QuerySet write guards ---------------------------------------

// bulk_insert_sql: placeholder_rows is list of rows, each row is already a
// joined "?, ?, ?" (or "%s, %s") string. Returns "VALUES (...), (...)" .
[[nodiscard]] std::string bulk_insert_sql(const std::vector<std::string>& row_sqls);

// Join one placeholder row: ["%s","%s"] → "%s, %s"
[[nodiscard]] std::string bulk_placeholder_row(const std::vector<std::string>& cols);

// Validate batch_size: None → ok (0); >0 → ok; else error.
// Returns 0=ok, 1=non-positive.
[[nodiscard]] int validate_positive_batch_size(bool is_none, int batch_size) noexcept;

// effective batch: min(user, max) if user set, else max. max<=0 → n_objs.
[[nodiscard]] int effective_batch_size(bool user_set, int user_batch, int max_batch,
                                       int n_objs) noexcept;

// QuerySet write preflight.
// flags bitfield built in Python; returns error code:
// 0=ok, 1=combinator, 2=sliced, 3=distinct_fields, 4=values/fields set
// Input: combinator, is_sliced, has_distinct_fields, has_values_fields
[[nodiscard]] int queryset_write_guard(bool combinator, bool is_sliced,
                                       bool has_distinct_fields,
                                       bool has_values_fields) noexcept;

// UPDATE SET "col" = %s fragments joined: "a = %s, b = %s"
[[nodiscard]] std::string sql_update_set_clause(
    const std::vector<std::string>& assignments);

// --- batched insert / deletion / get_or_create bookkeeping ------------------

// Multi-batch insert/update needs a wrapping atomic() when n_batches > 1.
[[nodiscard]] bool multi_batch_needs_atomic(int n_batches) noexcept;

// Keys that do not contain LOOKUP_SEP ("__") — used by get_or_create params.
[[nodiscard]] std::vector<std::string> keys_without_lookup_sep(
    const std::vector<std::string>& keys);

// True if key contains "__" (Django LOOKUP_SEP).
[[nodiscard]] bool key_has_lookup_sep(std::string_view key) noexcept;

// create_defaults for update_or_create: None → use update_defaults flag.
// Returns true if create_defaults should fall back to update_defaults.
[[nodiscard]] bool create_defaults_use_update(bool create_defaults_is_none) noexcept;

// --- workstreams 1-6: create / contains / combinators / save / collector / SQL ---

// Sort names and join with ", " (error messages for create / save).
[[nodiscard]] std::string join_sorted_comma(const std::vector<std::string>& names);

// bulk_create conflict kind: 0=none, 1=IGNORE, 2=UPDATE, -1=mutual exclusive.
[[nodiscard]] int bulk_create_conflict_kind(bool ignore_conflicts,
                                            bool update_conflicts) noexcept;

// contains() preflight after combinator check:
// 0=ok, 1=values/fields set, 2=unsaved (no pk).
[[nodiscard]] int contains_preflight(bool has_values_fields, bool pk_set) noexcept;

// aggregate() + distinct(fields) not implemented.
[[nodiscard]] bool aggregate_distinct_fields_error(bool has_distinct_fields) noexcept;

// filter/exclude after slice: has_filters && is_sliced.
[[nodiscard]] bool filter_after_slice_error(bool has_filters, bool is_sliced) noexcept;

// Prohibited filter kwargs among keys (returns matching subset, sorted).
[[nodiscard]] std::vector<std::string> prohibited_filter_kwargs(
    const std::vector<std::string>& keys);

// select_for_update: nowait and skip_locked conflict.
[[nodiscard]] bool select_for_update_options_conflict(bool nowait,
                                                      bool skip_locked) noexcept;

// union() when self is EmptyQuerySet: nonempty other count.
// 0=return self, 1=return single other, 2=combinator of multiple.
[[nodiscard]] int union_empty_self_kind(int nonempty_other_count) noexcept;

// intersection/difference: empty self returns self.
[[nodiscard]] bool combinator_return_empty_self(bool self_is_empty) noexcept;

// Model.save force_insert vs force_update/update_fields conflict.
[[nodiscard]] bool save_force_conflict(bool force_insert, bool force_update,
                                       bool has_update_fields) noexcept;

// Empty update_fields → skip save (update_fields is not None and empty).
[[nodiscard]] bool save_skip_empty_update_fields(bool update_fields_is_none,
                                                 int n_update_fields) noexcept;

// Cannot force update with no PK.
[[nodiscard]] bool save_force_update_no_pk(bool pk_set, bool force_update,
                                           bool has_update_fields) noexcept;

// Collector.add empty objs.
[[nodiscard]] bool collector_add_empty(int n_objs) noexcept;

// Collector.delete: no data and no fast_deletes → (0, {}).
[[nodiscard]] bool collector_delete_empty(int n_models, int n_fast_deletes) noexcept;

// Single-instance fast-delete path: one model, one instance.
[[nodiscard]] bool collector_single_fast_path(int n_models, int n_instances) noexcept;

// can_fast_delete result from precomputed bool flags (Python supplies model graph).
[[nodiscard]] bool can_fast_delete_result(bool from_field_blocks, bool model_ok,
                                          bool has_signal_listeners, bool parents_ok,
                                          bool relations_ok,
                                          bool no_bulk_related) noexcept;

// "quoted_col = rhs" / "quoted_col = NULL"
[[nodiscard]] std::string sql_assignment(std::string_view quoted_col,
                                         std::string_view rhs);
[[nodiscard]] std::string sql_null_assignment(std::string_view quoted_col);

// "(a, b, c)" for INSERT column list.
[[nodiscard]] std::string sql_parenthesized_list(const std::vector<std::string>& cols);

// "VALUES (placeholders)" single-row form.
[[nodiscard]] std::string sql_values_row(std::string_view placeholders);

// "SELECT select FROM (inner) subquery"
[[nodiscard]] std::string sql_aggregate_subquery(std::string_view select_sql,
                                                 std::string_view inner_sql);

// Space-join SQL fragments (INSERT/UPDATE result lists).
[[nodiscard]] std::string sql_space_join(const std::vector<std::string>& parts);

// row_count None → 0.
[[nodiscard]] int row_count_or_zero(bool is_none, int row_count) noexcept;

// --- workstreams 1-6 (chain / prefetch / dates / save / clean / utils) ------

// True if queryset is sliced (order_by/reverse/extra/earliest guards).
[[nodiscard]] bool queryset_sliced_error(bool is_sliced) noexcept;

// lookups/fields == (None,) → clear list.
[[nodiscard]] bool clear_none_arg(bool single_none) noexcept;

// only() rejects None.
[[nodiscard]] bool only_none_arg_error(bool single_none) noexcept;

// Flip standard_ordering for reverse().
[[nodiscard]] bool reverse_standard_ordering(bool standard_ordering) noexcept;

// __getitem__: 0=ok, 1=bad type, 2=negative.
[[nodiscard]] int queryset_index_validate(bool is_int, bool is_slice,
                                          bool has_negative) noexcept;

// & empty: 0=combine, 1=return other, 2=return self.
[[nodiscard]] int qs_and_empty_kind(bool self_empty, bool other_empty) noexcept;

// | / ^ empty: 0=combine, 1=return other (self empty), 2=return self (other empty).
[[nodiscard]] int qs_or_empty_kind(bool self_empty, bool other_empty) noexcept;

// dates/datetimes kind + order validation.
[[nodiscard]] bool date_kind_valid(std::string_view kind) noexcept;
[[nodiscard]] bool datetime_kind_valid(std::string_view kind) noexcept;
[[nodiscard]] bool date_order_valid(std::string_view order) noexcept;

// "DESC" → "-", else "" (for order_by field prefix).
[[nodiscard]] std::string order_by_desc_prefix(std::string_view order);

// earliest/latest: no fields and no get_latest_by → error.
[[nodiscard]] bool earliest_missing_fields(bool has_fields,
                                           bool has_get_latest_by) noexcept;

// save_base: parents present → atomic.
[[nodiscard]] bool save_base_needs_atomic(bool has_parents) noexcept;

// post_save created = not updated.
[[nodiscard]] bool save_created_flag(bool updated) noexcept;

// _do_update empty values: 0 → [()], 1 → [].
[[nodiscard]] int do_update_empty_values_kind(bool has_update_fields,
                                              bool exists) noexcept;

// clean_fields skip when name excluded or generated.
[[nodiscard]] bool clean_field_skip(bool name_in_exclude, bool generated) noexcept;

// Skip blank empty values.
[[nodiscard]] bool clean_field_skip_blank_empty(bool blank,
                                                bool in_empty_values) noexcept;

// ValidationError raise when errors non-empty.
[[nodiscard]] bool validation_has_errors(int n_error_keys) noexcept;

// NON_FIELD_ERRORS key ("__all__").
[[nodiscard]] bool is_non_field_errors_key(std::string_view name) noexcept;

// Fixed-offset timezone name: "+0530" / "-0800" from minutes.
[[nodiscard]] std::string fixed_timezone_name(int offset_minutes);

// is_aware / is_naive from utcoffset presence.
[[nodiscard]] bool datetime_is_aware(bool utcoffset_not_none) noexcept;
[[nodiscard]] bool datetime_is_naive(bool utcoffset_is_none) noexcept;

// mark_safe: 0=already safe (__html__), 1=callable decorator, 2=wrap SafeString.
[[nodiscard]] int mark_safe_kind(bool has_html, bool is_callable) noexcept;

// First path segment before LOOKUP_SEP ("__").
[[nodiscard]] std::string lookup_head(std::string_view lookup);

// --- full set 1-6: annotate / explain / refresh / unique / lookups / contrib ---

// QuerySet.ordered property.
[[nodiscard]] bool queryset_is_ordered(bool is_empty_qs, bool has_extra_order,
                                       bool has_order_by, bool default_ordering,
                                       bool has_meta_ordering,
                                       bool has_group_by) noexcept;

// Annotation alias conflicts with model field names.
[[nodiscard]] bool annotation_alias_conflicts(bool alias_in_names) noexcept;

// complex_filter: True if filter_obj is a Q (vs kwargs dict).
[[nodiscard]] bool complex_filter_is_q(bool is_q_instance) noexcept;

// raw()/using: using is None → fall back to self.db.
[[nodiscard]] bool using_is_none(bool using_is_none) noexcept;

// refresh_from_db: empty fields after stripping prefetched → early return.
[[nodiscard]] bool refresh_fields_empty(int n_fields) noexcept;

// refresh_from_db: any field name contains LOOKUP_SEP.
[[nodiscard]] bool refresh_fields_have_lookup_sep(
    const std::vector<std::string>& fields);

// unique_together check: any name in exclude → skip.
[[nodiscard]] bool unique_check_excluded(const std::vector<std::string>& check_names,
                                         const std::vector<std::string>& exclude);

// unique lookup value skip: None, or "" when empty-as-null.
[[nodiscard]] bool unique_lookup_skip_value(bool is_none, bool is_empty_str,
                                            bool empty_as_null) noexcept;

// unique check incomplete when some fields skipped.
[[nodiscard]] bool unique_check_incomplete(int n_check, int n_kwargs) noexcept;

// unique error key: single field → field name key; multi → non-field.
[[nodiscard]] bool unique_error_is_single_field(int n_check) noexcept;

// IN lookup with empty RHS after None discard.
[[nodiscard]] bool in_lookup_empty(int n_rhs) noexcept;

// BuiltinLookup: join "lhs op" already separate; "%s %s" space join of two.
[[nodiscard]] std::string sql_lhs_rhs(std::string_view lhs, std::string_view rhs_op);

// IN split OR groups: join with " OR ".
[[nodiscard]] std::string sql_or_join(const std::vector<std::string>& parts);

// Password usable: None or not starting with "!".
[[nodiscard]] bool is_password_usable(bool encoded_is_none,
                                      bool starts_with_unusable) noexcept;

// identify_hasher ancient formats: 0=split-alg, 1=unsalted_md5, 2=unsalted_sha1.
[[nodiscard]] int identify_hasher_kind(int encoded_len, bool has_dollar,
                                       bool starts_md5_dollar,
                                       bool starts_sha1_dollar) noexcept;

// Algorithm prefix before first "$".
[[nodiscard]] std::string hasher_algorithm_prefix(std::string_view encoded);

// Cache default key: "prefix:version:key".
[[nodiscard]] std::string cache_default_key(std::string_view key_prefix, int version,
                                            std::string_view key);

// Cache timeout kind: 0=use default_timeout, 1=expire-now (-1), 2=forever (None),
// 3=relative seconds (caller adds time.time()).
[[nodiscard]] int cache_timeout_kind(bool is_default_sentinel, bool is_none,
                                     int timeout) noexcept;

// File.multiple_chunks: size > chunk_size.
[[nodiscard]] bool file_multiple_chunks(std::int64_t size,
                                        std::int64_t chunk_size) noexcept;

// mask_hash: show first n chars, mask rest.
[[nodiscard]] std::string mask_hash(std::string_view hash, int show,
                                    char mask_char = '*');

// --- full set: QS internals / CSRF / security / URLs / template / i18n / schema ---

// QuerySet result cache already populated.
[[nodiscard]] bool result_cache_populated(bool cache_is_none) noexcept;

// Prefetch still needed after fill.
[[nodiscard]] bool prefetch_still_needed(bool has_lookups,
                                         bool prefetch_done) noexcept;

// Truthiness of a filled result cache.
[[nodiscard]] bool queryset_cache_truthy(int cache_len) noexcept;

// sticky_filter: return current sticky; caller clears.
[[nodiscard]] bool sticky_filter_active(bool sticky) noexcept;

// CSRF token length: 0=ok, 1=bad length (not secret_len or token_len).
[[nodiscard]] int csrf_token_length_ok(int len, int secret_len,
                                       int token_len) noexcept;

// CSRF allowed charset: all chars in [A-Za-z0-9].
[[nodiscard]] bool csrf_token_chars_valid(std::string_view token) noexcept;

// Unmask CSRF token (first half mask, second half cipher); charset is 62 alnum.
// token size must be 2 * secret_len. Empty on size mismatch.
[[nodiscard]] std::string csrf_unmask_token(std::string_view token, int secret_len);

// Mask secret with provided mask (same length as secret).
[[nodiscard]] std::string csrf_mask_secret(std::string_view secret,
                                           std::string_view mask);

// HSTS header value.
[[nodiscard]] std::string hsts_header_value(int seconds, bool include_subdomains,
                                            bool preload);

// https://host + full_path
[[nodiscard]] std::string https_redirect_url(std::string_view host,
                                             std::string_view full_path);

// Referrer-Policy join: already-stripped values comma-joined.
[[nodiscard]] std::string referrer_policy_header(
    const std::vector<std::string>& policies);

// Route looks like leftover regex (contains (?P< or starts ^ or ends $).
[[nodiscard]] bool route_looks_like_regex(std::string_view route) noexcept;

// Non-converter route match: 0=no, 1=endpoint exact, 2=prefix.
// remaining out-param via return of remaining path for prefix (empty for exact).
// Returns kind; remaining filled for kind 1/2.
struct RouteSimpleMatch {
  int kind = 0;  // 0=no, 1=exact, 2=prefix
  std::string remaining;
};
[[nodiscard]] RouteSimpleMatch route_simple_match(bool is_endpoint,
                                                  std::string_view route,
                                                  std::string_view path);

// Engine: app_dirs + explicit loaders is invalid.
[[nodiscard]] bool engine_loaders_app_dirs_conflict(bool app_dirs,
                                                    bool loaders_defined) noexcept;

// Template cache key when skip is empty: just template_name.
[[nodiscard]] std::string template_cache_key_plain(std::string_view template_name);

// to_language / to_locale
[[nodiscard]] std::string to_language(std::string_view locale);
[[nodiscard]] std::string to_locale(std::string_view language);

// Default gettext plural index: n != 1 → 1 else 0.
[[nodiscard]] int plural_index_default(int n) noexcept;

// language code length guard.
[[nodiscard]] bool language_code_too_long(int len, int max_len) noexcept;

// CREATE TABLE "name" (cols)
[[nodiscard]] std::string sql_create_table(std::string_view quoted_table,
                                           std::string_view columns_sql);

// Migration Operation.describe / formatted_description.
[[nodiscard]] std::string migration_describe(std::string_view class_name,
                                             std::string_view constructor_args);
[[nodiscard]] std::string migration_formatted_description(std::string_view category,
                                                          std::string_view description);

// column definition space-join (alias of sql_space_join for clarity at call sites).
// Use sql_space_join.

// --- full menu 1-12 ---------------------------------------------------------

// HTTP status in 100..599.
[[nodiscard]] bool http_status_code_valid(int code) noexcept;

// Strong ETag (starts with '"') → weak "W/" + etag; else unchanged.
[[nodiscard]] std::string weak_etag_if_strong(std::string_view etag);

// Accept-Encoding contains gzip as a word.
[[nodiscard]] bool accepts_gzip(std::string_view accept_encoding) noexcept;

// Skip gzip if content shorter than min_len.
[[nodiscard]] bool gzip_content_too_short(int content_len, int min_len = 200) noexcept;

// PREPEND_WWW: host non-empty and not already www.
[[nodiscard]] bool host_needs_www_prefix(std::string_view host) noexcept;

// scheme://www.host + path
[[nodiscard]] std::string www_redirect_url(std::string_view scheme,
                                           std::string_view host,
                                           std::string_view path);

// X-Frame-Options value uppercased; empty → "DENY".
[[nodiscard]] std::string xframe_options_value(std::string_view setting_value);

// Message.tags: join non-empty extra_tags + level_tag with space.
[[nodiscard]] std::string message_tags_join(std::string_view extra_tags,
                                            std::string_view level_tag);

// staticfiles hashed basename: root + "." + hash + ext (hash may include leading dot).
[[nodiscard]] std::string hashed_static_basename(std::string_view root,
                                                 std::string_view hash_with_dot,
                                                 std::string_view ext);

// Join dir + basename with '/' (posix).
[[nodiscard]] std::string posix_path_join(std::string_view directory,
                                          std::string_view basename);

// JSON indent separators marker: true → use (",", ": ").
[[nodiscard]] bool json_use_indent_separators(bool has_indent) noexcept;

// ISO datetime: replace trailing +00:00 with Z.
[[nodiscard]] std::string datetime_iso_utc_z(std::string_view iso);

// Header / address: contains CR or LF.
[[nodiscard]] bool string_has_newlines(std::string_view s) noexcept;

// Split email at last @: returns local@domain if exactly one @ structure ok.
// On failure empty local and domain.
struct EmailParts {
  std::string local;
  std::string domain;
  bool ok = false;
};
[[nodiscard]] EmailParts split_email_address(std::string_view address);

// app_label.ModelName
[[nodiscard]] std::string model_meta_label(std::string_view app_label,
                                           std::string_view object_name);

// app_label.model_name.manager
[[nodiscard]] std::string manager_str(std::string_view model_label,
                                      std::string_view manager_name);

// FromQuerySet class name: ManagerFromQuerySet
[[nodiscard]] std::string from_queryset_class_name(std::string_view manager_cls,
                                                   std::string_view qs_cls);

// Migration node key "app.migration_name"
[[nodiscard]] std::string migration_node_key(std::string_view app_label,
                                             std::string_view name);

// Permission codename "action_model"
[[nodiscard]] std::string perm_codename(std::string_view action,
                                        std::string_view model_name);

// user_can_authenticate: is_active missing (true) or active.
[[nodiscard]] bool user_can_authenticate(bool has_is_active,
                                         bool is_active) noexcept;

// Signal has any receivers.
[[nodiscard]] bool signal_has_receivers(int n_receivers) noexcept;

// import_string: split dotted path → module + attr (last dot).
// Returns false if no dot.
struct DottedPathParts {
  std::string module;
  std::string attr;
  bool ok = false;
};
[[nodiscard]] DottedPathParts split_dotted_path(std::string_view dotted);

// app_config.name + "." + module_to_search
[[nodiscard]] std::string app_module_path(std::string_view app_name,
                                          std::string_view submodule);

// Deprecation rename message.
[[nodiscard]] std::string renamed_method_warning(std::string_view class_name,
                                                 std::string_view old_name,
                                                 std::string_view new_name);

// File ends with .py (case-sensitive, Django style).
[[nodiscard]] bool path_ends_with_py(std::string_view path) noexcept;

// Archive/extension: path ends with any of suffixes (case-sensitive).
[[nodiscard]] bool path_has_any_suffix(
    std::string_view path, const std::vector<std::string>& suffixes);

// Postgres ArrayField path normalize for deconstruct.
[[nodiscard]] bool postgres_arrayfield_path_shorten(std::string_view path) noexcept;

// Content-Disposition attachment filename* packing helper: bare filename ok?
// Returns true if filename needs quoting (contains specials) — simplified.
[[nodiscard]] bool filename_needs_quotes(std::string_view filename) noexcept;

// --- menu 1-12: paginator / views / formsets / storage / handlers / ... -----

// num_pages: count==0 && !allow_empty → 0; else ceil(max(1,count-orphans)/per_page)
[[nodiscard]] int paginator_num_pages(int count, int per_page, int orphans,
                                      bool allow_empty_first_page) noexcept;

// page slice bottom = (number-1)*per_page
[[nodiscard]] int paginator_page_bottom(int number, int per_page) noexcept;

// top = min(bottom+per_page, count) with orphans pull on last page
[[nodiscard]] int paginator_page_top(int number, int per_page, int orphans,
                                     int count) noexcept;

// validate page number: 0=ok, 1=not integer path (caller), 2=min, 3=no results
// number already int; returns 0 if 1..num_pages, 2 if <1, 3 if >num_pages
[[nodiscard]] int paginator_number_range_code(int number, int num_pages) noexcept;

// resolve_url: relative path starts with ./ or ../
[[nodiscard]] bool url_is_relative_path(std::string_view to) noexcept;

// resolve_url fallback: looks like URL if contains / or .
[[nodiscard]] bool url_feels_like_url(std::string_view to) noexcept;

// formset total forms when bound: min(submitted, absolute_max)
[[nodiscard]] int formset_total_forms_bound(int submitted, int absolute_max) noexcept;

// formset total when unbound
[[nodiscard]] int formset_total_forms_unbound(int initial_forms, int min_num,
                                              int extra, int max_num) noexcept;

// storage path traversal: ".." in pure path parts (caller passes flag)
[[nodiscard]] bool path_has_dotdot(std::string_view path) noexcept;

// normalize name: backslash → slash
[[nodiscard]] std::string storage_normalize_name(std::string_view name);

// alternative name: root_XXXXXXX.ext (suffix provided by caller)
[[nodiscard]] std::string storage_alternative_name(std::string_view root,
                                                   std::string_view random7,
                                                   std::string_view ext);

// name available: !exists && !(max_length && len>max)
[[nodiscard]] bool storage_name_available(bool exists, bool has_max_length,
                                          int name_len, int max_length) noexcept;

// middleware must be sync or async capable
[[nodiscard]] bool middleware_capability_ok(bool sync_capable,
                                            bool async_capable) noexcept;

// contenttypes natural key join already model_meta_label

// sitemap priority clamp 0.0..1.0 as string "0.5" style — return whether valid
[[nodiscard]] bool sitemap_priority_valid(double priority) noexcept;

// changefreq allowed tokens
[[nodiscard]] bool sitemap_changefreq_valid(std::string_view freq) noexcept;

// ordinal suffix index: 0=th(11-13), else last digit 0-9 for templates
// returns 11 for 11-13 special, else value%10
[[nodiscard]] int ordinal_suffix_kind(int value) noexcept;

// intcomma: insert commas in integer string (ASCII, no l10n)
[[nodiscard]] std::string intcomma_ascii(std::string_view digits);

// check message serious: level >= threshold
[[nodiscard]] bool check_is_serious(int level, int threshold) noexcept;

// check id format packing already free-form

// test client path join query: path + ? + query if query non-empty
[[nodiscard]] std::string path_with_query(std::string_view path,
                                          std::string_view query);

// flatpages/redirects: ensure leading slash
[[nodiscard]] std::string ensure_leading_slash(std::string_view path);

// redirects: old_path == new_path invalid
[[nodiscard]] bool redirect_paths_equal(std::string_view a,
                                        std::string_view b) noexcept;

// GIS WKT-ish point packing: "POINT(x y)"
[[nodiscard]] std::string wkt_point(std::string_view x, std::string_view y);

// postgres array empty SQL: '{}'
[[nodiscard]] std::string postgres_empty_array_literal();

// dependency key already migration_node_key

// --- menu deep: generic views → syndication/test utils -----------------------

// List view default context name: model_name + "_list"
[[nodiscard]] std::string list_context_object_name(std::string_view model_name);

// request.method.lower() is one of http_method_names
[[nodiscard]] bool http_method_in_names(
    std::string_view method_lower, const std::vector<std::string>& names);

// Generic list page kwarg token "last"
[[nodiscard]] bool page_token_is_last(std::string_view page) noexcept;

// app/model_suffix.html template path fragment
[[nodiscard]] std::string model_template_name(std::string_view app_label,
                                              std::string_view object_name,
                                              std::string_view suffix);

// ModelForm factory class name: ModelName + "Form"
[[nodiscard]] std::string modelform_class_name(std::string_view model_name);

// construct_instance / model_to_dict field inclusion gate
[[nodiscard]] bool form_field_included(bool editable, bool fields_is_none,
                                       bool in_fields, bool exclude_active,
                                       bool in_exclude) noexcept;

// Admin URL primary-key quoting (QUOTE_MAP subset)
[[nodiscard]] std::string admin_quote(std::string_view s);

// lookup key ends with suffix (e.g. "__in", "__isnull")
[[nodiscard]] bool lookup_key_endswith(std::string_view key,
                                       std::string_view suffix) noexcept;

// prepare_lookup_value for __isnull: lower not in ("", "false", "0")
[[nodiscard]] bool prepare_lookup_isnull(std::string_view value_lower) noexcept;

// LoginView path-equals (redirect loop guard)
[[nodiscard]] bool paths_equal(std::string_view a,
                               std::string_view b) noexcept;

// ASCII case-insensitive equality (after caller NFKC if needed)
[[nodiscard]] bool strings_ci_equal_ascii(std::string_view a,
                                          std::string_view b) noexcept;

// MigrationWriter.filename
[[nodiscard]] std::string migration_filename(std::string_view name);

// Introspection TableInfo type == "t"
[[nodiscard]] bool introspection_is_table(std::string_view type_code) noexcept;

// CombinedExpression: (lhs connector rhs)
[[nodiscard]] std::string combined_expression_sql(std::string_view lhs,
                                                  std::string_view connector,
                                                  std::string_view rhs);

// SQLite Decimal cast wrap
[[nodiscard]] std::string sql_cast_as_numeric(std::string_view sql);

// File cache expiry: exp is None → false; else exp < now
[[nodiscard]] bool cache_timestamp_expired(bool exp_is_none, double exp,
                                           double now) noexcept;

// md5hex + suffix file basename
[[nodiscard]] std::string cache_file_name(std::string_view hexdigest,
                                          std::string_view suffix);

[[nodiscard]] bool cache_cull_needed(int num_entries,
                                     int max_entries) noexcept;

// sample size for cull; 0 means clear-all path (cull_frequency==0)
[[nodiscard]] int cache_cull_sample_size(int num_entries,
                                         int cull_frequency) noexcept;

// WSGIRequest.path assembly
[[nodiscard]] std::string wsgi_request_path(std::string_view script_name,
                                            std::string_view path_info);

// Map exception kind → status: "http404","permission","bad","suspicious","other"
// Returns 404/403/400/400/500
[[nodiscard]] int exception_status_code(std::string_view kind) noexcept;

// Postgres full-text helpers
// Empty return means None (blank after strip)
[[nodiscard]] std::string postgres_normalize_spaces(std::string_view val);
[[nodiscard]] std::string postgres_psql_escape(std::string_view query);
[[nodiscard]] std::string search_vector_match_sql(std::string_view lhs,
                                                  std::string_view rhs);

// Syndication add_domain pieces
[[nodiscard]] std::string feed_protocol(bool secure) noexcept;
[[nodiscard]] bool feed_url_is_network_path(std::string_view url) noexcept;
[[nodiscard]] bool feed_url_has_scheme(std::string_view url) noexcept;
[[nodiscard]] std::string feed_network_path_url(std::string_view protocol,
                                                std::string_view url);
[[nodiscard]] std::string feed_absolute_url(std::string_view protocol,
                                            std::string_view domain,
                                            std::string_view url);

// admindocs
[[nodiscard]] std::string dotted_qualname(std::string_view module,
                                          std::string_view qualname);
[[nodiscard]] std::string strip_p_tags(std::string_view value);

// test.utils.Approximate
[[nodiscard]] bool approximate_equal(double val, double other,
                                      int places) noexcept;

// Allow: GET, POST header join
[[nodiscard]] std::string http_allow_header(
    const std::vector<std::string>& methods);

// MEDIA_URL style trailing slash
[[nodiscard]] std::string ensure_trailing_slash(std::string_view url);

// Autodetector / state model name lower
[[nodiscard]] std::string string_ascii_lower(std::string_view s);

// management command name from argv subcommand path basename without .py
[[nodiscard]] std::string management_command_name(std::string_view path);

// ASGI path_info: path with script_name prefix removed (literal prefix)
[[nodiscard]] std::string asgi_path_info(std::string_view path,
                                         std::string_view script_name);

// Fieldsets field name already free-form

// --- menu 1-12 unit-testable footholds --------------------------------------

// Field.__str__: model_label + "." + name
[[nodiscard]] std::string field_str(std::string_view model_label,
                                    std::string_view name);

// Field.__repr__: name optional → "<path: name>" or "<path>"
[[nodiscard]] std::string field_repr(std::string_view path,
                                     bool has_name, std::string_view name);

// verbose_name from name: '_' → ' '
[[nodiscard]] std::string verbose_name_from_name(std::string_view name);

// Field name check codes: 0=ok, 1=endswith_, 2=contains__, 3=is_pk
[[nodiscard]] int field_name_check_code(std::string_view name) noexcept;

// attname/column: db_column if non-empty else attname
[[nodiscard]] std::string field_column_name(std::string_view attname,
                                            std::string_view db_column);

// Aggregate.default_alias
[[nodiscard]] std::string aggregate_default_alias(std::string_view expr_name,
                                                  std::string_view agg_name);

// "DISTINCT " prefix when distinct
[[nodiscard]] std::string sql_distinct_prefix(bool distinct);

// Index column with order: "-col" or "col"
[[nodiscard]] std::string index_column_with_order(std::string_view column,
                                                  bool descending);

// Index name leading digit/_ → D + rest (after first char dropped for _)
// Actually: if name[0]=='_' or digit: "D" + name[1:]
[[nodiscard]] std::string index_name_fix_leading(std::string_view name);

// Admin ChangeList: result_count <= list_max_show_all
[[nodiscard]] bool admin_can_show_all(int result_count,
                                      int list_max_show_all) noexcept;
[[nodiscard]] bool admin_is_multi_page(int result_count,
                                       int list_per_page) noexcept;

// "?encoded"
[[nodiscard]] std::string query_string_with_prefix(std::string_view encoded);

// CSS class join for admin labels
[[nodiscard]] std::string css_classes_join(const std::vector<std::string>& classes);

// Password reset token: "ts-hash"
[[nodiscard]] std::string password_reset_token_join(std::string_view ts_b36,
                                                    std::string_view hash_hex);
// Split token: returns (ok, ts_b36, rest). rest may contain more dashes.
// ok=false if no dash.
struct TokenSplit {
  bool ok;
  std::string ts_b36;
  std::string rest;
};
[[nodiscard]] TokenSplit password_reset_token_split(std::string_view token);

// min length check
[[nodiscard]] bool password_meets_min_length(int password_len,
                                             int min_length) noexcept;

// Numeric-only password (all digits)
[[nodiscard]] bool password_is_numeric_only(std::string_view password) noexcept;

// Migration graph Node.__repr__
[[nodiscard]] std::string migration_node_repr(std::string_view cls,
                                              std::string_view app,
                                              std::string_view name);

// Serializer simple: caller uses Python repr; pack import lines
[[nodiscard]] std::string serializer_datetime_import();

// Sitemap absolute + page
[[nodiscard]] std::string sitemap_absolute_url(std::string_view protocol,
                                               std::string_view domain,
                                               std::string_view path);
[[nodiscard]] std::string sitemap_paged_url(std::string_view absolute_url,
                                            int page);
[[nodiscard]] std::string x_robots_tag_value();

// Session middleware: save only if status < 500
[[nodiscard]] bool http_status_session_saveable(int status_code) noexcept;

// Static was_modified: return true if should send body (modified/missing header)
// header_missing OR mtime > header_mtime
[[nodiscard]] bool resource_was_modified(bool header_missing, double mtime,
                                         double header_mtime) noexcept;

// Error page HTML fragment packing (title + details into fixed skeleton is large;
// just pack request_path quote already elsewhere)

// Template library: register name = explicit or func_name
[[nodiscard]] std::string template_register_name(std::string_view explicit_name,
                                                 std::string_view func_name);

// Apps model lookup key
// string_ascii_lower already exists

// test.html normalize whitespace (ASCII runs → single space, no strip ends? re.sub)
// ASCII_WHITESPACE.sub(" ", string) — does not strip, replaces each run
[[nodiscard]] std::string normalize_ascii_whitespace(std::string_view s);

// Boolean attribute normalize: value empty or equals name → treat as bool
[[nodiscard]] bool html_boolean_attr_is_true(std::string_view name,
                                             std::string_view value) noexcept;

// Func SQL template simple: FUNCTION(args)
[[nodiscard]] std::string sql_func_call(std::string_view function,
                                        std::string_view expressions);

// get_FOO_display method name
[[nodiscard]] std::string field_display_method_name(std::string_view field_name);

// Constraint / index suffix packing already free-form

// Auth middleware: login_required attr default true when missing handled in py

// Messages: level constants already ints

// Migrations optimizer: empty list length
[[nodiscard]] bool optimizer_lists_equal_len(int a, int b) noexcept;

// --- Tier A/B unit-testable footholds ----------------------------------------

// related_name ends with '+'
[[nodiscard]] bool related_name_ends_plus(std::string_view name) noexcept;

// related_name is valid Python-ish identifier (ASCII letters/digits/_ , not
// starting digit) — keyword check stays in Python
[[nodiscard]] bool related_name_is_identifier(std::string_view name) noexcept;

// reverse query name ends with '_'
[[nodiscard]] bool related_query_name_ends_underscore(
    std::string_view name) noexcept;

// reverse query contains LOOKUP_SEP "__"
[[nodiscard]] bool related_query_name_has_lookup_sep(
    std::string_view name) noexcept;

// FK default name: model_name + "_" + pk_name
[[nodiscard]] std::string fk_default_name(std::string_view model_name,
                                          std::string_view pk_name);

// forward related filter key: field__rh_field
[[nodiscard]] std::string related_filter_key(std::string_view field_name,
                                             std::string_view rh_field);

// constraint deconstruct path shorten
[[nodiscard]] std::string constraint_deconstruct_path(std::string_view path);

// varchar type: max_length None → "varchar" else "varchar(N)"
[[nodiscard]] std::string sql_varchar_type(bool has_max_length, int max_length);

// decimal type packing
[[nodiscard]] std::string sql_decimal_type(int max_digits, int decimal_places);

// admin selectfilter class
[[nodiscard]] std::string admin_selectfilter_class(bool is_stacked);

// admin site repr
[[nodiscard]] std::string admin_site_repr(std::string_view cls,
                                          std::string_view name);

// Permission.__str__: "ct | name"
[[nodiscard]] std::string permission_str(std::string_view content_type,
                                         std::string_view name);

// facet count key: "{i}__c"
[[nodiscard]] std::string admin_facet_count_key(int index);

// extract lookup name lower for datetime Extract
[[nodiscard]] std::string extract_lookup_name(std::string_view lookup);

// Now() / CURRENT_TIMESTAMP style
[[nodiscard]] std::string sql_now_sqlite();
[[nodiscard]] std::string sql_now_postgresql();

// tag URI packing (hostname, date_suffix, path, fragment)
[[nodiscard]] std::string feed_tag_uri(std::string_view hostname,
                                       std::string_view date_suffix,
                                       std::string_view path,
                                       std::string_view fragment);

// progress bar fraction
[[nodiscard]] int progress_percent(int count, int total) noexcept;
[[nodiscard]] int progress_done_width(int percent, int width) noexcept;

// backend vendor known
[[nodiscard]] bool backend_vendor_is(std::string_view vendor,
                                     std::string_view expected) noexcept;

// isolation level normalize lower
// string_ascii_lower already exists

// management prog display: basename + " " + subcommand
[[nodiscard]] std::string management_prog(std::string_view basename,
                                          std::string_view subcommand);

// FileField default max_length
[[nodiscard]] int filefield_default_max_length() noexcept;

// JSONField internal type
[[nodiscard]] std::string jsonfield_internal_type();

// test label is path-like (contains / or ends with .py)
[[nodiscard]] bool test_label_looks_like_path(std::string_view label) noexcept;

// debug template name join
[[nodiscard]] std::string debug_template_path(std::string_view name);

// date view year advance: just validate year+1 in range for C++ int
[[nodiscard]] bool date_year_in_range(int year) noexcept;

// mysql FOUND_ROWS style flag bookkeeping — skip

// Truncate SQL join table list already free-form

// collectstatic skip — path normalize already exists

// Generic FK: no column
// field_column_name already handles empty db_column

// UniqueConstraint name packing
[[nodiscard]] std::string unique_constraint_name(std::string_view model,
                                                 std::string_view fields_joined);

// http host is unix socket (starts with /)
[[nodiscard]] bool db_host_is_unix_socket(std::string_view host) noexcept;

// set_config timezone SQL for postgres
[[nodiscard]] std::string postgres_set_timezone_sql();

// mysql isolation levels valid token
[[nodiscard]] bool mysql_isolation_level_valid(std::string_view level) noexcept;

// serializer m2m error packing skip

// ContentType natural key path already model_meta_label

// Tier 1 ORM: simple SELECT col1, col2 FROM table WHERE col = %s LIMIT n
// table/cols/where_col must already be quoted by the backend.
[[nodiscard]] std::string simple_select_eq_limit_sql(
    std::string_view quoted_table, const std::vector<std::string>& quoted_cols,
    std::string_view quoted_where_col, int limit);

}  // namespace django::native
