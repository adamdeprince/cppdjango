// Date / time / duration parsing (Django regex fallback paths + full duration).
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace django::native {

struct ParsedDate {
  int year;
  int month;
  int day;
};

struct ParsedTime {
  int hour;
  int minute;
  int second = 0;
  int microsecond = 0;
};

struct ParsedDateTime {
  int year;
  int month;
  int day;
  int hour = 0;
  int minute = 0;
  int second = 0;
  int microsecond = 0;
  // Minutes east of UTC. nullopt = naive; 0 = UTC/Z.
  std::optional<int> tz_offset_minutes;
};

struct ParsedDuration {
  // Total microseconds (signed), easier than multi-field for timedelta.
  long long total_microseconds = 0;
};

// Regex-path parsers. nullopt => not well-formatted.
// throw std::invalid_argument => well-formatted but invalid (e.g. month 56).
[[nodiscard]] std::optional<ParsedDate> parse_date(std::string_view value);
[[nodiscard]] std::optional<ParsedTime> parse_time(std::string_view value);
[[nodiscard]] std::optional<ParsedDateTime> parse_datetime(std::string_view value);
[[nodiscard]] std::optional<ParsedDuration> parse_duration(std::string_view value);

}  // namespace django::native
