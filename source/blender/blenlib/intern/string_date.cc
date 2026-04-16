/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/** \file
 * \ingroup bli
 */

#include <algorithm>
#include <array>
#include <iomanip>
#include <string>

#include <fmt/format.h>

#include "BLI_string_date.hh"

#include "BLT_translation.hh"

namespace blender::date_format {

struct LocalePatterns {
  StringRef locale;
  StringRef date;
  StringRef time;
};

static const LocalePatterns *get_locale_patterns(const StringRef locale_iso)
{
  static constexpr std::array<LocalePatterns, 9> patterns = {{
      {"", "{d:02} {b} {Y}", "{H:02}:{M:02}"},         /* default */
      {"en_US", "{d:02} {b} {Y}", "{I}:{M:02} {p}"},   /* English (US) */
      {"ar_EG", "{d:02} {b} {Y}", "{I}:{M} {p}"},      /* Arabic (Egypt) */
      {"zh_HANS", "{Y}年{m}月{d}日", "{H:02}:{M:02}"}, /* Chinese (Simplified) */
      {"zh_HANT", "{Y}年{m}月{d}日", "{H:02}:{M:02}"}, /* Chinese (Traditional) */
      {"hu_HU", "{Y}. {b} {d:02}", "{H:02}:{M:02}"},   /* Hungarian */
      {"ja_JP", "{Y}年{m}月{d}日", "{H:02}:{M:02}"},   /* Japanese */
      {"ko_KR", "{Y}년 {m}월 {d}일", "{H:02}:{M:02}"}, /* Korean */
      {"ur", "{d:02} {b} {Y}", "{I}:{M} {p}"},         /* Urdu */
  }};

  for (const LocalePatterns &pattern : patterns) {
    if (pattern.locale == locale_iso) {
      return &pattern;
    }
  }

  /* Fallback default pattern (index 0). */
  return &patterns[0];
}

static std::string format_with_pattern(const std::tm *tm, std::string pattern)
{
  static constexpr std::array<StringRef, 12> months = {CTX_N_(BLT_I18NCONTEXT_TIME, "Jan"),
                                                       CTX_N_(BLT_I18NCONTEXT_TIME, "Feb"),
                                                       CTX_N_(BLT_I18NCONTEXT_TIME, "Mar"),
                                                       CTX_N_(BLT_I18NCONTEXT_TIME, "Apr"),
                                                       CTX_N_(BLT_I18NCONTEXT_TIME, "May"),
                                                       CTX_N_(BLT_I18NCONTEXT_TIME, "Jun"),
                                                       CTX_N_(BLT_I18NCONTEXT_TIME, "Jul"),
                                                       CTX_N_(BLT_I18NCONTEXT_TIME, "Aug"),
                                                       CTX_N_(BLT_I18NCONTEXT_TIME, "Sep"),
                                                       CTX_N_(BLT_I18NCONTEXT_TIME, "Oct"),
                                                       CTX_N_(BLT_I18NCONTEXT_TIME, "Nov"),
                                                       CTX_N_(BLT_I18NCONTEXT_TIME, "Dec")};

  BLI_assert(tm->tm_mon >= 0 && tm->tm_mon < 12);
  const int month_index = std::clamp(tm->tm_mon, 0, 11);

  return fmt::format(fmt::runtime(pattern),
                     fmt::arg("Y", tm->tm_year + 1900),
                     fmt::arg("m", tm->tm_mon + 1),
                     fmt::arg("b", CTX_IFACE_(BLT_I18NCONTEXT_TIME, months[month_index])),
                     fmt::arg("d", tm->tm_mday),
                     fmt::arg("H", tm->tm_hour),
                     fmt::arg("M", tm->tm_min),
                     fmt::arg("I", (tm->tm_hour % 12) == 0 ? 12 : (tm->tm_hour % 12)),
                     fmt::arg("p",
                              (tm->tm_hour < 12) ? CTX_IFACE_(BLT_I18NCONTEXT_TIME, "AM") :
                                                   CTX_IFACE_(BLT_I18NCONTEXT_TIME, "PM")));
}

/* Public functions. */

std::string time(const std::tm *date_time, const StringRef locale_iso)
{
  const LocalePatterns *pattern = get_locale_patterns(locale_iso);
  return format_with_pattern(date_time, pattern->time);
}

std::string date(const std::tm *date_time, const StringRef locale_iso)
{
  const LocalePatterns *pattern = get_locale_patterns(locale_iso);
  return format_with_pattern(date_time, pattern->date);
}

std::string datetime(const std::tm *datetime,
                     const StringRef locale_iso,
                     const std::tm *now,
                     const StringRef today,
                     const StringRef yesterday)
{
  bool is_today = false;
  bool is_yesterday = false;
  if (now && !today.is_empty() && !yesterday.is_empty()) {
    is_today = (datetime->tm_yday == now->tm_yday && datetime->tm_year == now->tm_year);
    std::tm yesterday_tm = *now;
    yesterday_tm.tm_mday--;
    mktime(&yesterday_tm);
    is_yesterday = (datetime->tm_yday == yesterday_tm.tm_yday &&
                    datetime->tm_year == yesterday_tm.tm_year);
  }

  const std::string time_s = time(datetime, locale_iso);

  if (is_today) {
    return std::string(today) + " " + time_s;
  }
  else if (is_yesterday) {
    return std::string(yesterday) + " " + time_s;
  }
  else {
    const std::string date_s = date(datetime, locale_iso);
    return date_s + " " + time_s;
  }
}

}  // namespace blender::date_format
