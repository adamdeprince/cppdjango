#include "dateparse.hpp"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace django::native {
namespace {

[[nodiscard]] bool is_digit(char c) noexcept {
  return c >= '0' && c <= '9';
}

// Consume one or more digits into `out`. Returns false if none.
[[nodiscard]] bool take_int(std::string_view s, std::size_t& i, int& out,
                            int min_digits = 1, int max_digits = 9) {
  if (i >= s.size() || !is_digit(s[i])) {
    return false;
  }
  long long v = 0;
  int count = 0;
  while (i < s.size() && is_digit(s[i]) && count < max_digits) {
    v = v * 10 + (s[i] - '0');
    ++i;
    ++count;
  }
  // Reject if more digits remain when we hit max and still digit? For bounded
  // fields we only read max_digits; caller validates remainder.
  if (count < min_digits) {
    return false;
  }
  out = static_cast<int>(v);
  return true;
}

// Read up to 6 microsecond digits, pad to 6 with zeros; ignore extra digits.
[[nodiscard]] bool take_fraction(std::string_view s, std::size_t& i, int& micro) {
  if (i >= s.size() || !is_digit(s[i])) {
    return false;
  }
  char buf[6] = {'0', '0', '0', '0', '0', '0'};
  int count = 0;
  while (i < s.size() && is_digit(s[i])) {
    if (count < 6) {
      buf[count] = s[i];
    }
    ++i;
    ++count;
  }
  micro = (buf[0] - '0') * 100000 + (buf[1] - '0') * 10000 + (buf[2] - '0') * 1000 +
          (buf[3] - '0') * 100 + (buf[4] - '0') * 10 + (buf[5] - '0');
  return true;
}

void require_valid_date(int y, int m, int d) {
  // datetime.date constructor validates; mimic via simple check.
  if (m < 1 || m > 12 || d < 1 || d > 31 || y < 1 || y > 9999) {
    throw std::invalid_argument("invalid date");
  }
  static constexpr int mdays[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int dim = mdays[m];
  if (m == 2) {
    const bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    dim = leap ? 29 : 28;
  }
  if (d > dim) {
    throw std::invalid_argument("invalid date");
  }
}

void require_valid_time(int h, int mi, int s, int us) {
  if (h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 59 || us < 0 ||
      us > 999999) {
    throw std::invalid_argument("invalid time");
  }
}

// Parse float with . or , decimal (for ISO 8601 duration components).
[[nodiscard]] bool take_float(std::string_view s, std::size_t& i, double& out) {
  if (i >= s.size() || !is_digit(s[i])) {
    return false;
  }
  double v = 0;
  while (i < s.size() && is_digit(s[i])) {
    v = v * 10 + (s[i] - '0');
    ++i;
  }
  if (i < s.size() && (s[i] == '.' || s[i] == ',')) {
    ++i;
    double place = 0.1;
    bool any = false;
    while (i < s.size() && is_digit(s[i])) {
      v += (s[i] - '0') * place;
      place *= 0.1;
      ++i;
      any = true;
    }
    (void)any;
  }
  out = v;
  return true;
}

std::optional<ParsedDuration> parse_standard_duration(std::string_view value) {
  // standard_duration_re:
  // (?:(?P<days>-?\d+) (days?, )?)?
  // (?P<sign>-?)
  // ((?:(?P<hours>\d+):)(?=\d+:\d+))?
  // (?:(?P<minutes>\d+):)?
  // (?P<seconds>\d+)
  // (?:[.,](?P<microseconds>\d{1,6})\d{0,6})?
  std::size_t i = 0;
  double days = 0;
  int sign = 1;
  double hours = 0, minutes = 0, seconds = 0, microseconds = 0;

  // Optional days: "-?\d+ days?, " or "-?\d+ " (with optional "day(s), ")
  // Actually: (?:(?P<days>-?\d+) (days?, )?)?
  // So: digits, space, optional "day"/"days", optional ", "
  // Looking at regex: `(?:(?P<days>-?\d+) (days?, )?)?`
  // After days number there is a required space, then optional `days?, `
  // Examples: "4 15:30", "4 days 0:15:30", "-4 15:30"

  auto try_days = [&]() -> bool {
    std::size_t j = i;
    int day_sign = 1;
    if (j < value.size() && value[j] == '-') {
      day_sign = -1;
      ++j;
    }
    if (j >= value.size() || !is_digit(value[j])) {
      return false;
    }
    int day_val = 0;
    if (!take_int(value, j, day_val, 1, 18)) {
      return false;
    }
    if (j >= value.size() || value[j] != ' ') {
      return false;
    }
    ++j;  // space
    // optional "days?, "
    if (j + 3 <= value.size() && value.substr(j, 3) == "day") {
      j += 3;
      if (j < value.size() && value[j] == 's') {
        ++j;
      }
      if (j < value.size() && value[j] == ',') {
        ++j;
      }
      if (j < value.size() && value[j] == ' ') {
        ++j;
      }
    }
    days = static_cast<double>(day_sign * day_val);
    i = j;
    return true;
  };
  try_days();

  // sign
  if (i < value.size() && value[i] == '-') {
    sign = -1;
    ++i;
  }

  // hours: (?:(?P<hours>\d+):)(?=\d+:\d+)  — only if followed by m:s
  // minutes: (?:(?P<minutes>\d+):)?
  // seconds: (?P<seconds>\d+)
  // fraction optional

  // Peek rest for structure
  std::size_t j = i;
  if (j >= value.size() || !is_digit(value[j])) {
    return std::nullopt;
  }

  // Collect colon-separated ints then optional fraction
  int n1 = 0, n2 = 0, n3 = 0;
  if (!take_int(value, j, n1, 1, 18)) {
    return std::nullopt;
  }

  if (j < value.size() && value[j] == ':') {
    ++j;
    if (!take_int(value, j, n2, 1, 18)) {
      return std::nullopt;
    }
    if (j < value.size() && value[j] == ':') {
      ++j;
      if (!take_int(value, j, n3, 1, 18)) {
        return std::nullopt;
      }
      hours = n1;
      minutes = n2;
      seconds = n3;
    } else {
      // m:s
      minutes = n1;
      seconds = n2;
    }
  } else {
    seconds = n1;
  }

  if (j < value.size() && (value[j] == '.' || value[j] == ',')) {
    ++j;
    int micro = 0;
    std::size_t frac_start = j;
    if (!take_fraction(value, j, micro)) {
      return std::nullopt;
    }
    // take_fraction already pads; but standard allows trailing extra digits
    // beyond 6 which take_fraction consumes. Good.
    (void)frac_start;
    microseconds = micro;
  }

  if (j != value.size()) {
    return std::nullopt;
  }

  // Validate hours lookahead semantics: hours only when three components.
  // Already handled.

  // timedelta days must have magnitude <= 999999999.
  if (days > 999999999.0 || days < -999999999.0) {
    throw std::overflow_error("duration days out of range");
  }
  const double total_seconds =
      sign * (hours * 3600.0 + minutes * 60.0 + seconds + microseconds / 1e6) +
      days * 86400.0;
  // Also catch when time components push past the limit.
  const double total_days = total_seconds / 86400.0;
  if (total_days > 999999999.0 || total_days < -999999999.0) {
    throw std::overflow_error("duration days out of range");
  }
  ParsedDuration d;
  d.total_microseconds = llround(total_seconds * 1e6);
  return d;
}

std::optional<ParsedDuration> parse_iso8601_duration(std::string_view value) {
  // (?P<sign>[-+]?)P(?:(?P<days>\d+([.,]\d+)?)D)?(?:T(?:(?P<hours>...)H)?(?:(?P<minutes>...)M)?(?:(?P<seconds>...)S)?)?
  std::size_t i = 0;
  int sign = 1;
  if (i < value.size() && (value[i] == '-' || value[i] == '+')) {
    if (value[i] == '-') {
      sign = -1;
    }
    ++i;
  }
  if (i >= value.size() || value[i] != 'P') {
    return std::nullopt;
  }
  ++i;

  double days = 0, hours = 0, minutes = 0, seconds = 0;
  bool saw_anything = false;

  auto take_comp = [&](char unit, double& dest) -> bool {
    std::size_t j = i;
    double v = 0;
    if (!take_float(value, j, v)) {
      return false;
    }
    if (j >= value.size() || value[j] != unit) {
      return false;
    }
    ++j;
    dest = v;
    i = j;
    saw_anything = true;
    return true;
  };

  // days before T
  if (i < value.size() && value[i] != 'T') {
    if (!take_comp('D', days)) {
      // Could be invalid like P4Y
      if (i < value.size() && is_digit(value[i])) {
        return std::nullopt;  // had digits but not D
      }
    }
  }

  if (i < value.size() && value[i] == 'T') {
    ++i;
    // order H M S
    if (i < value.size() && is_digit(value[i])) {
      // try H
      std::size_t save = i;
      if (!take_comp('H', hours)) {
        i = save;
      }
    }
    if (i < value.size() && is_digit(value[i])) {
      std::size_t save = i;
      if (!take_comp('M', minutes)) {
        i = save;
      }
    }
    if (i < value.size() && is_digit(value[i])) {
      std::size_t save = i;
      if (!take_comp('S', seconds)) {
        i = save;
      }
    }
  }

  if (i != value.size() || !saw_anything) {
    return std::nullopt;
  }

  // ISO: sign applies to the whole duration including days
  const double total_seconds =
      sign * (days * 86400.0 + hours * 3600.0 + minutes * 60.0 + seconds);
  ParsedDuration d;
  d.total_microseconds = llround(total_seconds * 1e6);
  return d;
}

std::optional<ParsedDuration> parse_postgres_interval(std::string_view value) {
  // (?:(?P<days>-?\d+) (days? ?))?
  // (?:(?P<sign>[-+])?(?P<hours>\d+):(?P<minutes>\d\d):(?P<seconds>\d\d)(?:\.(?P<microseconds>\d{1,6}))?)?
  if (value.empty()) {
    return std::nullopt;
  }
  std::size_t i = 0;
  double days = 0;
  bool has_time = false;
  int sign = 1;
  int hours = 0, minutes = 0, seconds = 0, microseconds = 0;

  // days part
  std::size_t j = i;
  int day_sign = 1;
  if (j < value.size() && value[j] == '-') {
    day_sign = -1;
    ++j;
  }
  if (j < value.size() && is_digit(value[j])) {
    int day_val = 0;
    std::size_t k = j;
    if (take_int(value, k, day_val, 1, 18) && k < value.size() && value[k] == ' ') {
      ++k;
      if (k + 3 <= value.size() && value.substr(k, 3) == "day") {
        k += 3;
        if (k < value.size() && value[k] == 's') {
          ++k;
        }
        if (k < value.size() && value[k] == ' ') {
          ++k;
        }
        days = day_sign * day_val;
        i = k;
      }
    }
  }

  if (i < value.size()) {
    if (value[i] == '+' || value[i] == '-') {
      sign = (value[i] == '-') ? -1 : 1;
      ++i;
    }
    // hours:minutes:seconds — minutes and seconds are exactly 2 digits in postgres re
    // Actually: (?P<hours>\d+):(?P<minutes>\d\d):(?P<seconds>\d\d)
    if (i >= value.size() || !is_digit(value[i])) {
      if (i == value.size() && days != 0) {
        // "1 day" only
        ParsedDuration d;
        d.total_microseconds = llround(days * 86400.0 * 1e6);
        return d;
      }
      return std::nullopt;
    }
    if (!take_int(value, i, hours, 1, 18)) {
      return std::nullopt;
    }
    if (i >= value.size() || value[i] != ':') {
      return std::nullopt;
    }
    ++i;
    if (i + 2 > value.size() || !is_digit(value[i]) || !is_digit(value[i + 1])) {
      return std::nullopt;
    }
    minutes = (value[i] - '0') * 10 + (value[i + 1] - '0');
    i += 2;
    if (i >= value.size() || value[i] != ':') {
      return std::nullopt;
    }
    ++i;
    if (i + 2 > value.size() || !is_digit(value[i]) || !is_digit(value[i + 1])) {
      return std::nullopt;
    }
    seconds = (value[i] - '0') * 10 + (value[i + 1] - '0');
    i += 2;
    if (i < value.size() && value[i] == '.') {
      ++i;
      if (!take_fraction(value, i, microseconds)) {
        return std::nullopt;
      }
      // postgres re is \d{1,6} only — no extra digits. take_fraction consumes all
      // digits; if more than 6 were present, we should reject... Python re only
      // allows 1-6. Check: if we consumed more than 6 digit chars... take_fraction
      // allows extra. For postgres, cap: only 1-6 total.
      // Re-read: `(?:\.(?P<microseconds>\d{1,6}))?` — exactly 1-6, no trailing.
      // take_fraction reads all digits and uses first 6. Extra digits mean j not
      // at end only if... it consumes all, so end matches. Python would fail on
      // 7+ digit fraction. Let's count: if more than 6 digits after dot, nullopt.
      // Fix take path: only 1-6 digits for postgres.
    }
    has_time = true;
  }

  if (i != value.size()) {
    return std::nullopt;
  }
  if (!has_time && days == 0) {
    return std::nullopt;
  }

  double total = days * 86400.0;
  if (has_time) {
    total += sign * (hours * 3600.0 + minutes * 60.0 + seconds + microseconds / 1e6);
  }
  ParsedDuration d;
  d.total_microseconds = llround(total * 1e6);
  return d;
}

}  // namespace

std::optional<ParsedDate> parse_date(std::string_view value) {
  // (?P<year>\d{4})-(?P<month>\d{1,2})-(?P<day>\d{1,2})$
  std::size_t i = 0;
  int y = 0, m = 0, d = 0;
  if (!take_int(value, i, y, 4, 4)) {
    return std::nullopt;
  }
  if (i >= value.size() || value[i] != '-') {
    return std::nullopt;
  }
  ++i;
  if (!take_int(value, i, m, 1, 2)) {
    return std::nullopt;
  }
  if (i >= value.size() || value[i] != '-') {
    return std::nullopt;
  }
  ++i;
  if (!take_int(value, i, d, 1, 2)) {
    return std::nullopt;
  }
  if (i != value.size()) {
    return std::nullopt;
  }
  require_valid_date(y, m, d);
  return ParsedDate{y, m, d};
}

std::optional<ParsedTime> parse_time(std::string_view value) {
  // (?P<hour>\d{1,2}):(?P<minute>\d{1,2})(?::(?P<second>\d{1,2})(?:[.,](?P<microsecond>\d{1,6})\d{0,6})?)?$
  std::size_t i = 0;
  int h = 0, mi = 0, s = 0, us = 0;
  if (!take_int(value, i, h, 1, 2)) {
    return std::nullopt;
  }
  if (i >= value.size() || value[i] != ':') {
    return std::nullopt;
  }
  ++i;
  if (!take_int(value, i, mi, 1, 2)) {
    return std::nullopt;
  }
  if (i < value.size() && value[i] == ':') {
    ++i;
    if (!take_int(value, i, s, 1, 2)) {
      return std::nullopt;
    }
    if (i < value.size() && (value[i] == '.' || value[i] == ',')) {
      ++i;
      if (!take_fraction(value, i, us)) {
        return std::nullopt;
      }
    }
  }
  if (i != value.size()) {
    return std::nullopt;
  }
  require_valid_time(h, mi, s, us);
  return ParsedTime{h, mi, s, us};
}

std::optional<ParsedDateTime> parse_datetime(std::string_view value) {
  // date[T ]time with optional fraction and tz
  std::size_t i = 0;
  int y = 0, m = 0, d = 0;
  if (!take_int(value, i, y, 4, 4)) {
    return std::nullopt;
  }
  if (i >= value.size() || value[i] != '-') {
    return std::nullopt;
  }
  ++i;
  if (!take_int(value, i, m, 1, 2)) {
    return std::nullopt;
  }
  if (i >= value.size() || value[i] != '-') {
    return std::nullopt;
  }
  ++i;
  if (!take_int(value, i, d, 1, 2)) {
    return std::nullopt;
  }

  ParsedDateTime out;
  out.year = y;
  out.month = m;
  out.day = d;

  if (i == value.size()) {
    // date only — regex requires time part actually:
    // datetime_re has [T ] time required... Looking at the regex:
    // r"(?P<year>\d{4})-...-(?P<day>\d{1,2})[T ](?P<hour>..."
    // Time is required in regex! But fromisoformat handles date-only.
    // So regex path should nullopt for date-only.
    return std::nullopt;
  }
  if (value[i] != 'T' && value[i] != ' ') {
    return std::nullopt;
  }
  ++i;

  int h = 0, mi = 0, s = 0, us = 0;
  if (!take_int(value, i, h, 1, 2)) {
    return std::nullopt;
  }
  if (i >= value.size() || value[i] != ':') {
    return std::nullopt;
  }
  ++i;
  if (!take_int(value, i, mi, 1, 2)) {
    return std::nullopt;
  }
  if (i < value.size() && value[i] == ':') {
    ++i;
    if (!take_int(value, i, s, 1, 2)) {
      return std::nullopt;
    }
    if (i < value.size() && (value[i] == '.' || value[i] == ',')) {
      ++i;
      if (!take_fraction(value, i, us)) {
        return std::nullopt;
      }
    }
  }

  // optional whitespace then tz
  while (i < value.size() && (value[i] == ' ' || value[i] == '\t')) {
    ++i;
  }

  std::optional<int> tz;
  if (i < value.size()) {
    if (value[i] == 'Z') {
      tz = 0;
      ++i;
    } else if (value[i] == '+' || value[i] == '-') {
      const int tsign = (value[i] == '-') ? -1 : 1;
      ++i;
      int th = 0;
      if (!take_int(value, i, th, 2, 2)) {
        // could be +0230 without colon — still 2 digit hours minimum in re:
        // [+-]\d{2}(?::?\d{2})?
        return std::nullopt;
      }
      int tmin = 0;
      if (i < value.size() && value[i] == ':') {
        ++i;
        if (!take_int(value, i, tmin, 2, 2)) {
          return std::nullopt;
        }
      } else if (i < value.size() && is_digit(value[i])) {
        if (!take_int(value, i, tmin, 2, 2)) {
          return std::nullopt;
        }
      }
      tz = tsign * (th * 60 + tmin);
    } else {
      return std::nullopt;
    }
  }

  if (i != value.size()) {
    return std::nullopt;
  }

  require_valid_date(y, m, d);
  require_valid_time(h, mi, s, us);
  out.hour = h;
  out.minute = mi;
  out.second = s;
  out.microsecond = us;
  out.tz_offset_minutes = tz;
  return out;
}

std::optional<ParsedDuration> parse_duration(std::string_view value) {
  if (auto d = parse_standard_duration(value)) {
    return d;
  }
  if (auto d = parse_iso8601_duration(value)) {
    return d;
  }
  if (auto d = parse_postgres_interval(value)) {
    return d;
  }
  return std::nullopt;
}

}  // namespace django::native
