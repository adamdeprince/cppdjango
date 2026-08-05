// Locale-adjacent formatters: numberformat, dateformat, timesince partials.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace django::native {

// django.utils.numberformat.format core (string path after number→str).
// grouping_intervals: empty + use_grouping false → no grouping.
// For single-value grouping N, pass {N, 0}. For locale sequences, pass as-is.
[[nodiscard]] std::string format_number(
    std::string_view number, std::string_view decimal_sep,
    std::optional<int> decimal_pos, const std::vector<int>& grouping_intervals,
    std::string_view thousand_sep, bool use_grouping);

// PHP-style date/time format (django.utils.dateformat).
// Localized name tables are 1-based for months (index 0 unused) and 0=Monday
// for weekdays (Django WEEKDAYS keys).
struct DateFormatInput {
  int year = 0;
  int month = 1;  // 1-12
  int day = 1;
  int hour = 0;
  int minute = 0;
  int second = 0;
  int microsecond = 0;
  bool has_time = true;   // false for date-only (reject time format chars)
  bool has_tz = false;
  // True when the datetime instance itself is timezone-aware (for ISO 'c').
  // Distinct from has_tz, which is also true for naive when a default TZ applies.
  bool is_aware = false;
  std::string tz_name;    // T — active timezone name
  std::string e_name;     // e — only instance tzinfo name (empty if naive)
  int utc_offset_seconds = 0;  // Z; O uses same
  int is_dst = -1;        // I: -1 unknown → ""; 0/1
  // Pre-translated names (Python gettext). Empty → English fallbacks.
  std::vector<std::string> months;       // size 13, [1..12]
  std::vector<std::string> months_3;     // size 13
  std::vector<std::string> months_alt;   // size 13
  std::vector<std::string> months_ap;    // size 13
  std::vector<std::string> weekdays;     // size 7, Mon=0
  std::vector<std::string> weekdays_abbr;
  std::string am;   // "a.m."
  std::string pm;   // "p.m."
  std::string AM;
  std::string PM;
  std::string midnight;
  std::string noon;
};

// Throws std::invalid_argument if time format char used on date-only input.
// Format char 'r' is not implemented (RFC5322) — returns empty optional to
// signal Python fallback. 'c' uses ISO-like assembly; 'U' needs timestamp
// provided via optional unix_timestamp.
[[nodiscard]] std::optional<std::string> php_date_format(
    const DateFormatInput& in, std::string_view format_string,
    std::optional<std::int64_t> unix_timestamp = std::nullopt);

// timesince arithmetic: years, months, weeks, days, hours, minutes.
// Returns empty vector when since <= 0 (caller shows "0 minutes").
// Otherwise returns non-zero leading partials up to `depth` (keys order:
// year, month, week, day, hour, minute) as pairs (unit_index, count).
// unit_index: 0=year … 5=minute.
[[nodiscard]] std::vector<std::pair<int, int>> timesince_partials(
    int y1, int m1, int d1, int h1, int mi1, int s1, int y2, int m2, int d2,
    int h2, int mi2, int s2, int depth);

// avoid_wrapping: space → NBSP
[[nodiscard]] std::string avoid_wrapping(std::string_view value);

// filesizeformat unit selection.
// unit: 0=bytes, 1=KB, 2=MB, 3=GB, 4=TB, 5=PB
// For unit 0, use abs_bytes; for unit>=1, use scaled (already / unit).
struct FileSizeParts {
  bool negative = false;
  int unit = 0;
  std::int64_t abs_bytes = 0;
  double scaled = 0.0;
};
[[nodiscard]] FileSizeParts filesize_parts(std::int64_t bytes);

}  // namespace django::native
