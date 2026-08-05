#include "locale.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>

namespace django::native {
namespace {

[[nodiscard]] bool is_leap(int year) noexcept {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

[[nodiscard]] int days_in_month(int year, int month) noexcept {
  static constexpr int kDays[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && is_leap(year)) {
    return 29;
  }
  if (month < 1 || month > 12) {
    return 30;
  }
  return kDays[month];
}

// Monday=0 … Sunday=6 (Python datetime.weekday())
[[nodiscard]] int weekday_mon0(int year, int month, int day) noexcept {
  // Sakamoto
  static constexpr int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  int y = year;
  if (month < 3) {
    --y;
  }
  const int w = (y + y / 4 - y / 100 + y / 400 + t[month - 1] + day) % 7;
  // Sakamoto: 0=Sunday … 6=Saturday → convert to Mon=0
  return (w + 6) % 7;
}

[[nodiscard]] int day_of_year(int year, int month, int day) noexcept {
  static constexpr int cum[] = {0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  int n = cum[month] + day;
  if (month > 2 && is_leap(year)) {
    ++n;
  }
  return n;
}

// Days from civil date (Howard Hinnant), epoch 1970-01-01 = 0 is not required;
// we only need differences. Returns proleptic Gregorian day count.
[[nodiscard]] std::int64_t days_from_civil(int y, unsigned m, unsigned d) noexcept {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy =
      (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<std::int64_t>(era) * 146097 +
         static_cast<std::int64_t>(doe) - 719468;
}

// Monday of ISO week 1 for year y (as day count).
[[nodiscard]] std::int64_t isoweek1monday(int y) noexcept {
  // Jan 4 is always in week 1; find its Monday.
  const int jan4_wday = weekday_mon0(y, 1, 4);  // Mon=0
  return days_from_civil(y, 1, 4) - jan4_wday;
}

// Floor division toward -∞ (C++ / truncates toward 0).
[[nodiscard]] std::int64_t floor_div(std::int64_t a, std::int64_t b) noexcept {
  std::int64_t q = a / b;
  const std::int64_t r = a % b;
  if (r != 0 && ((r < 0) != (b < 0))) {
    --q;
  }
  return q;
}

// ISO calendar year/week (matches datetime.date.isocalendar())
void iso_calendar(int year, int month, int day, int& iso_year, int& iso_week) {
  const std::int64_t today = days_from_civil(year, month, day);
  std::int64_t week1monday = isoweek1monday(year);
  std::int64_t week = floor_div(today - week1monday, 7);
  if (week < 0) {
    iso_year = year - 1;
    week1monday = isoweek1monday(iso_year);
    week = floor_div(today - week1monday, 7);
  } else {
    const std::int64_t next_week1 = isoweek1monday(year + 1);
    if (today >= next_week1) {
      iso_year = year + 1;
      week = floor_div(today - next_week1, 7);
    } else {
      iso_year = year;
    }
  }
  iso_week = static_cast<int>(week) + 1;
}

[[nodiscard]] std::string pad2(int v) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%02d", v);
  return buf;
}

[[nodiscard]] std::string pad4(int v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d", v);
  return buf;
}

[[nodiscard]] std::string pad6(int v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%06d", v);
  return buf;
}

[[nodiscard]] const std::string& name_at(const std::vector<std::string>& names,
                                         std::size_t idx,
                                         std::string_view fallback) {
  static thread_local std::string tmp;
  if (idx < names.size() && !names[idx].empty()) {
    return names[idx];
  }
  tmp.assign(fallback);
  return tmp;
}

// English fallbacks
constexpr const char* kMonths[] = {
    "",        "January", "February", "March",    "April",    "May",
    "June",    "July",    "August",   "September", "October", "November",
    "December"};
constexpr const char* kMonths3[] = {"",    "jan", "feb", "mar", "apr", "may", "jun",
                                    "jul", "aug", "sep", "oct", "nov", "dec"};
constexpr const char* kMonthsAP[] = {
    "",     "Jan.",  "Feb.",  "March", "April", "May",  "June",
    "July", "Aug.",  "Sept.", "Oct.",  "Nov.",  "Dec."};
constexpr const char* kWeekdays[] = {"Monday", "Tuesday",  "Wednesday", "Thursday",
                                     "Friday", "Saturday", "Sunday"};
constexpr const char* kWeekdaysAbbr[] = {"Mon", "Tue", "Wed", "Thu",
                                         "Fri", "Sat", "Sun"};

}  // namespace

std::string format_number(std::string_view number, std::string_view decimal_sep,
                          std::optional<int> decimal_pos,
                          const std::vector<int>& grouping_intervals,
                          std::string_view thousand_sep, bool use_grouping) {
  if (number.empty()) {
    return {};
  }
  std::string str_number(number);
  std::string sign;
  if (!str_number.empty() && str_number[0] == '-') {
    sign = "-";
    str_number.erase(0, 1);
  }
  std::string int_part;
  std::string dec_part;
  const auto dot = str_number.find('.');
  if (dot != std::string::npos) {
    int_part = str_number.substr(0, dot);
    dec_part = str_number.substr(dot + 1);
    if (decimal_pos.has_value()) {
      if (static_cast<int>(dec_part.size()) > *decimal_pos) {
        dec_part.resize(static_cast<std::size_t>(*decimal_pos));
      }
    }
  } else {
    int_part = str_number;
    dec_part.clear();
  }
  if (decimal_pos.has_value()) {
    while (static_cast<int>(dec_part.size()) < *decimal_pos) {
      dec_part += '0';
    }
  }
  std::string dec_out;
  if (!dec_part.empty()) {
    dec_out = std::string(decimal_sep);
    dec_out += dec_part;
  }

  if (use_grouping && !grouping_intervals.empty() && grouping_intervals[0] != 0) {
    std::vector<int> intervals = grouping_intervals;
    int active = intervals.front();
    intervals.erase(intervals.begin());
    std::string int_part_gd;
    int cnt = 0;
    for (auto it = int_part.rbegin(); it != int_part.rend(); ++it) {
      if (cnt && cnt == active) {
        if (!intervals.empty()) {
          const int next = intervals.front();
          intervals.erase(intervals.begin());
          if (next != 0) {
            active = next;
          }
          // Python: active_interval = intervals.pop(0) or active_interval
          // so 0 keeps previous active
        }
        // thousand_sep reversed then reverse whole — append sep reversed
        for (auto sit = thousand_sep.rbegin(); sit != thousand_sep.rend(); ++sit) {
          int_part_gd += *sit;
        }
        cnt = 0;
      }
      int_part_gd += *it;
      ++cnt;
    }
    int_part.assign(int_part_gd.rbegin(), int_part_gd.rend());
  }
  return sign + int_part + dec_out;
}

std::optional<std::string> php_date_format(const DateFormatInput& in,
                                           std::string_view format_string,
                                           std::optional<std::int64_t> unix_timestamp) {
  // Time-related format chars (from TimeFormat methods)
  auto is_time_char = [](char c) {
    switch (c) {
      case 'a':
      case 'A':
      case 'e':
      case 'f':
      case 'g':
      case 'G':
      case 'h':
      case 'H':
      case 'i':
      case 'O':
      case 'P':
      case 's':
      case 'T':
      case 'u':
      case 'Z':
        return true;
      default:
        return false;
    }
  };

  const int wday = weekday_mon0(in.year, in.month, in.day);
  int iso_year = in.year, iso_week = 1;
  iso_calendar(in.year, in.month, in.day, iso_year, iso_week);
  const int yday = day_of_year(in.year, in.month, in.day);
  const int dim = days_in_month(in.year, in.month);

  auto month_name = [&](const std::vector<std::string>& names, const char* const* eng)
      -> std::string {
    if (in.month >= 1 && in.month <= 12) {
      if (static_cast<std::size_t>(in.month) < names.size() &&
          !names[static_cast<std::size_t>(in.month)].empty()) {
        return names[static_cast<std::size_t>(in.month)];
      }
      return eng[in.month];
    }
    return {};
  };

  auto weekday_name = [&](const std::vector<std::string>& names, const char* const* eng)
      -> std::string {
    if (wday >= 0 && wday < 7) {
      if (static_cast<std::size_t>(wday) < names.size() &&
          !names[static_cast<std::size_t>(wday)].empty()) {
        return names[static_cast<std::size_t>(wday)];
      }
      return eng[wday];
    }
    return {};
  };

  auto fmt_char = [&](char c) -> std::string {
    if (!in.has_time && is_time_char(c)) {
      throw std::invalid_argument(
          std::string("The format for date objects may not contain "
                      "time-related format specifiers (found '") +
          c + "').");
    }
    switch (c) {
      case 'a':
        return in.hour > 11 ? (in.pm.empty() ? "p.m." : in.pm)
                            : (in.am.empty() ? "a.m." : in.am);
      case 'A':
        return in.hour > 11 ? (in.PM.empty() ? "PM" : in.PM)
                            : (in.AM.empty() ? "AM" : in.AM);
      case 'b':
        return month_name(in.months_3, kMonths3);
      case 'c': {
        // Match datetime.isoformat(): date[Ttime][.ffffff][±HH:MM]
        char buf[80];
        int n = 0;
        if (in.has_time) {
          if (in.microsecond) {
            n = std::snprintf(buf, sizeof(buf),
                              "%04d-%02d-%02dT%02d:%02d:%02d.%06d", in.year,
                              in.month, in.day, in.hour, in.minute, in.second,
                              in.microsecond);
          } else {
            n = std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
                              in.year, in.month, in.day, in.hour, in.minute,
                              in.second);
          }
        } else {
          n = std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", in.year,
                            in.month, in.day);
        }
        if (n < 0) {
          n = 0;
        }
        // isoformat() only includes an offset when the instance is aware.
        if (in.is_aware && static_cast<std::size_t>(n) + 7 < sizeof(buf)) {
          int secs = in.utc_offset_seconds;
          const char sign = secs < 0 ? '-' : '+';
          if (secs < 0) {
            secs = -secs;
          }
          const int hh = secs / 3600;
          const int mm = (secs / 60) % 60;
          std::snprintf(buf + n, sizeof(buf) - static_cast<std::size_t>(n),
                        "%c%02d:%02d", sign, hh, mm);
        }
        return buf;
      }
      case 'd':
        return pad2(in.day);
      case 'D':
        return weekday_name(in.weekdays_abbr, kWeekdaysAbbr);
      case 'E':
        return month_name(in.months_alt, kMonths);
      case 'e':
        // Only the datetime's own tzinfo name (empty for naive).
        return in.e_name;
      case 'f': {
        int hour = in.hour % 12;
        if (hour == 0) {
          hour = 12;
        }
        if (in.minute) {
          char buf[16];
          std::snprintf(buf, sizeof(buf), "%d:%02d", hour, in.minute);
          return buf;
        }
        return std::to_string(hour);
      }
      case 'F':
        return month_name(in.months, kMonths);
      case 'g': {
        int hour = in.hour % 12;
        if (hour == 0) {
          hour = 12;
        }
        return std::to_string(hour);
      }
      case 'G':
        return std::to_string(in.hour);
      case 'h': {
        int hour = in.hour % 12;
        if (hour == 0) {
          hour = 12;
        }
        return pad2(hour);
      }
      case 'H':
        return pad2(in.hour);
      case 'i':
        return pad2(in.minute);
      case 'I':
        if (!in.has_tz || in.is_dst < 0) {
          return "";
        }
        return in.is_dst ? "1" : "0";
      case 'j':
        return std::to_string(in.day);
      case 'l':
        return weekday_name(in.weekdays, kWeekdays);
      case 'L':
        return is_leap(in.year) ? "True" : "False";
      case 'm':
        return pad2(in.month);
      case 'M': {
        std::string m = month_name(in.months_3, kMonths3);
        if (!m.empty() && m[0] >= 'a' && m[0] <= 'z') {
          m[0] = static_cast<char>(m[0] - 'a' + 'A');
        }
        return m;
      }
      case 'n':
        return std::to_string(in.month);
      case 'N':
        return month_name(in.months_ap, kMonthsAP);
      case 'o':
        return std::to_string(iso_year);
      case 'O': {
        if (!in.has_tz) {
          return "";
        }
        int seconds = in.utc_offset_seconds;
        const char sign = seconds < 0 ? '-' : '+';
        seconds = std::abs(seconds);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%c%02d%02d", sign, seconds / 3600,
                      (seconds / 60) % 60);
        return buf;
      }
      case 'P': {
        if (in.minute == 0 && in.hour == 0) {
          return in.midnight.empty() ? "midnight" : in.midnight;
        }
        if (in.minute == 0 && in.hour == 12) {
          return in.noon.empty() ? "noon" : in.noon;
        }
        {
          int hour12 = in.hour % 12;
          if (hour12 == 0) {
            hour12 = 12;
          }
          std::string fpart;
          if (in.minute) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d:%02d", hour12, in.minute);
            fpart = buf;
          } else {
            fpart = std::to_string(hour12);
          }
          const std::string ap =
              in.hour > 11 ? (in.pm.empty() ? "p.m." : in.pm)
                           : (in.am.empty() ? "a.m." : in.am);
          return fpart + " " + ap;
        }
      }
      case 'r':
        // Signal Python fallback for RFC 5322
        throw std::runtime_error("php_date_format: r requires Python");
      case 's':
        return pad2(in.second);
      case 'S': {
        if (in.day == 11 || in.day == 12 || in.day == 13) {
          return "th";
        }
        switch (in.day % 10) {
          case 1:
            return "st";
          case 2:
            return "nd";
          case 3:
            return "rd";
          default:
            return "th";
        }
      }
      case 't':
        return std::to_string(dim);
      case 'T':
        return in.has_tz ? in.tz_name : "";
      case 'u':
        return pad6(in.microsecond);
      case 'U':
        if (unix_timestamp.has_value()) {
          return std::to_string(*unix_timestamp);
        }
        throw std::runtime_error("php_date_format: U needs timestamp");
      case 'w':
        return std::to_string((wday + 1) % 7);
      case 'W':
        return std::to_string(iso_week);
      case 'y':
        return pad2(in.year % 100);
      case 'Y':
        return pad4(in.year);
      case 'z':
        return std::to_string(yday);
      case 'Z':
        if (!in.has_tz) {
          return "";
        }
        return std::to_string(in.utc_offset_seconds);
      default:
        return std::string(1, c);
    }
  };

  // Split like re_formatchars: (?<!\\)([aAbcdDeEfFgGhHiIjlLmMnNoOPrsStTUuwWyYzZ])
  static constexpr std::string_view kChars =
      "aAbcdDeEfFgGhHiIjlLmMnNoOPrsStTUuwWyYzZ";
  std::string out;
  for (std::size_t i = 0; i < format_string.size(); ++i) {
    if (format_string[i] == '\\' && i + 1 < format_string.size()) {
      out += format_string[i + 1];
      ++i;
      continue;
    }
    const char c = format_string[i];
    if (kChars.find(c) != std::string_view::npos) {
      out += fmt_char(c);
    } else {
      out += c;
    }
  }
  return out;
}

std::vector<std::pair<int, int>> timesince_partials(int y1, int m1, int d1, int h1,
                                                    int mi1, int s1, int y2, int m2,
                                                    int d2, int h2, int mi2, int s2,
                                                    int depth) {
  // d = t1, now = t2 (caller swaps for timeuntil)
  // since in seconds
  // Use approximate day count via pivot algorithm matching Django.

  auto to_seconds_of_day = [](int h, int mi, int s) {
    return h * 3600 + mi * 60 + s;
  };

  // delta days roughly via civil_from_days
  auto days_from_civil = [](int y, int m, int d) -> std::int64_t {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy =
        (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(d) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 +
           static_cast<std::int64_t>(doe) - 719468;
  };

  const std::int64_t day1 = days_from_civil(y1, m1, d1);
  const std::int64_t day2 = days_from_civil(y2, m2, d2);
  const int sod1 = to_seconds_of_day(h1, mi1, s1);
  const int sod2 = to_seconds_of_day(h2, mi2, s2);
  std::int64_t since =
      (day2 - day1) * 86400 + static_cast<std::int64_t>(sod2 - sod1);
  if (since <= 0) {
    return {};
  }

  int total_months = (y2 - y1) * 12 + (m2 - m1);
  if (d1 > d2 || (d1 == d2 && sod1 > sod2)) {
    --total_months;
  }
  int years = total_months / 12;
  int months = total_months % 12;

  // pivot
  int pivot_year = y1 + years;
  int pivot_month = m1 + months;
  if (pivot_month > 12) {
    pivot_month -= 12;
    pivot_year += 1;
  }
  const int pivot_day = std::min(days_in_month(pivot_year, pivot_month), d1);
  const std::int64_t pivot_days = days_from_civil(pivot_year, pivot_month, pivot_day);
  const std::int64_t remaining =
      (day2 - pivot_days) * 86400 + static_cast<std::int64_t>(sod2 - sod1);

  static constexpr int kChunks[] = {
      60 * 60 * 24 * 7,  // week
      60 * 60 * 24,      // day
      60 * 60,           // hour
      60,                // minute
  };
  int weeks = 0, days = 0, hours = 0, minutes = 0;
  double rem = static_cast<double>(remaining);
  if (rem < 0) {
    rem = 0;
  }
  weeks = static_cast<int>(rem / kChunks[0]);
  rem -= weeks * kChunks[0];
  days = static_cast<int>(rem / kChunks[1]);
  rem -= days * kChunks[1];
  hours = static_cast<int>(rem / kChunks[2]);
  rem -= hours * kChunks[2];
  minutes = static_cast<int>(rem / kChunks[3]);

  const int partials[] = {years, months, weeks, days, hours, minutes};
  int i = 0;
  for (; i < 6; ++i) {
    if (partials[i] != 0) {
      break;
    }
  }
  if (i == 6) {
    return {};
  }

  std::vector<std::pair<int, int>> out;
  int current_depth = 0;
  while (i < 6 && current_depth < depth) {
    if (partials[i] == 0) {
      break;
    }
    out.emplace_back(i, partials[i]);
    ++current_depth;
    ++i;
  }
  return out;
}

std::string avoid_wrapping(std::string_view value) {
  // U+00A0 NO-BREAK SPACE as UTF-8: C2 A0
  std::string out;
  out.reserve(value.size() + 8);
  for (unsigned char c : value) {
    if (c == ' ') {
      out += "\xc2\xa0";
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}

FileSizeParts filesize_parts(std::int64_t bytes) {
  FileSizeParts p;
  p.negative = bytes < 0;
  std::int64_t absb = p.negative ? -bytes : bytes;
  p.abs_bytes = absb;
  constexpr std::int64_t KB = 1LL << 10;
  constexpr std::int64_t MB = 1LL << 20;
  constexpr std::int64_t GB = 1LL << 30;
  constexpr std::int64_t TB = 1LL << 40;
  constexpr std::int64_t PB = 1LL << 50;
  if (absb < KB) {
    p.unit = 0;
    p.scaled = static_cast<double>(absb);
  } else if (absb < MB) {
    p.unit = 1;
    p.scaled = static_cast<double>(absb) / static_cast<double>(KB);
  } else if (absb < GB) {
    p.unit = 2;
    p.scaled = static_cast<double>(absb) / static_cast<double>(MB);
  } else if (absb < TB) {
    p.unit = 3;
    p.scaled = static_cast<double>(absb) / static_cast<double>(GB);
  } else if (absb < PB) {
    p.unit = 4;
    p.scaled = static_cast<double>(absb) / static_cast<double>(TB);
  } else {
    p.unit = 5;
    p.scaled = static_cast<double>(absb) / static_cast<double>(PB);
  }
  return p;
}

}  // namespace django::native
