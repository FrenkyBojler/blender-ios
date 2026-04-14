/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#include <algorithm>
#include <iomanip>
#include <string>

#include <fmt/format.h>

#include "BLI_map.hh"
#include "BLI_string_date.hh"

#include "BLT_translation.hh"

namespace blender::date_format {

static const char *months[12] = {CTX_N_(BLT_I18NCONTEXT_SHORTMONTH, "Jan"),
                                 CTX_N_(BLT_I18NCONTEXT_SHORTMONTH, "Feb"),
                                 CTX_N_(BLT_I18NCONTEXT_SHORTMONTH, "Mar"),
                                 CTX_N_(BLT_I18NCONTEXT_SHORTMONTH, "Apr"),
                                 CTX_N_(BLT_I18NCONTEXT_SHORTMONTH, "May"),
                                 CTX_N_(BLT_I18NCONTEXT_SHORTMONTH, "Jun"),
                                 CTX_N_(BLT_I18NCONTEXT_SHORTMONTH, "Jul"),
                                 CTX_N_(BLT_I18NCONTEXT_SHORTMONTH, "Aug"),
                                 CTX_N_(BLT_I18NCONTEXT_SHORTMONTH, "Sep"),
                                 CTX_N_(BLT_I18NCONTEXT_SHORTMONTH, "Oct"),
                                 CTX_N_(BLT_I18NCONTEXT_SHORTMONTH, "Nov"),
                                 CTX_N_(BLT_I18NCONTEXT_SHORTMONTH, "Dec")};

struct CLDRPatterns {
  const char date[25];
  const char time[18];
};

static const Map<std::string, CLDRPatterns> &locale_patterns = *([]() {
  return new Map<std::string, CLDRPatterns>{
      {"default", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},
      {"en_US", {"{d:02} {b} {Y}", "{I}:{M:02} {p}"}},      /* English (US).*/
      {"en_GB", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},       /* English (UK). */
      {"ar_EG", {"{d:02} {b} {Y}", "{I}:{M} {p}"}},         /* Arabic (Egypt). */
      {"eu_EU", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},       /* Basque. */
      {"bg_BG", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},       /* Bulgarian. */
      {"ca_AD", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},       /* Catalan. */
      {"zh_HANS", {"{Y}年{m}月{d}日", "{H:02}:{M:02}"}},    /* Chinese (Simplified). */
      {"zh_HANT", {"{Y}年{m}月{d}日", "{H:02}:{M:02}"}},    /* Chinese (Traditional). */
      {"hr", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},          /* Croatian. */
      {"cs_CZ", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},       /* Czech. */
      {"da", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},          /* Danish. */
      {"nl_NL", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},       /* Dutch - Nederlands. */
      {"fi_FI", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},       /* Finnish. */
      {"fr_FR", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},       /* French. */
      {"ka", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},          /* Georgian. */
      {"de_DE", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},       /* German. */
      {"el_GR", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},       /* Greek. */
      {"he_IL", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},       /* Hebrew. */
      {"hi_IN", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},       /* Hindi. */
      {"hu_HU", {"{Y}. {b} {d:02}", "{H:02}:{M:02}"}},      /* Hungarian. */
      {"id_ID", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},       /* Indonesian. */
      {"it_IT", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},       /* Italian. */
      {"ja_JP", {"{Y}年{m}月{d}日", "{H:02}:{M:02}"}},      /* Japanese. */
      {"ko_KR", {"{Y}년 {m}월{d}일", "{H:02}:{M:02}"}},     /* Korean. */
      {"nb", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},          /* Norwegian. */
      {"fa_IR", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},       /* Persian. */
      {"pl_PL", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},       /* Polish. */
      {"pt_BR", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},       /* Portuguese (Brazil). */
      {"pt_PT", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},       /* Portuguese (Portugal). */
      {"ro_RO", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},       /* Romanian. */
      {"ru_RU", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},       /* Russian. */
      {"sr_RS", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},       /* Serbian (Cyrillic). */
      {"sr_RS@latin", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}}, /* Serbian (Latin). */
      {"sk_SK", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},       /* Slovak. */
      {"sl", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},          /* Slovenian. */
      {"es", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},          /* Spanish. */
      {"sv_SE", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},       /* Swedish. */
      {"sw", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},          /* Swahili. */
      {"ta", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},          /* Tamil. */
      {"th_TH", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},       /* Thai. */
      {"tr_TR", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},       /* Turkish. */
      {"uk_UA", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},       /* Ukrainian. */
      {"ur", {"{d:02} {b} {Y}", "{I}:{M} {p}"}},            /* Urdu. */
      {"vi_VN", {"{d:02} {b} {Y}", "{H:02}:{M:02}"}},       /* Vietnamese. */
  };
}());

static const CLDRPatterns *get_locale_patterns(const char *locale_iso)
{
  if (locale_iso == nullptr || locale_iso[0] == '\0') {
    return locale_patterns.lookup_ptr("default");
  }

  const CLDRPatterns *pattern = locale_patterns.lookup_ptr(locale_iso);
  if (pattern) {
    return pattern;
  }

  std::string loc(locale_iso);
  if (loc.size() >= 2) {
    std::string lang = loc.substr(0, 2);
    pattern = locale_patterns.lookup_ptr(lang);
    if (pattern) {
      return pattern;
    }
  }

  return locale_patterns.lookup_ptr("default");
}

static std::string format_with_pattern(const std::tm *tm,
                                       const std::string &pattern,
                                       const char *locale_iso)
{
  BLI_assert(tm->tm_mon >= 0 && tm->tm_mon < 12);
  const int month_index = std::clamp(tm->tm_mon, 0, 11);

  return fmt::format(fmt::runtime(pattern),
                     fmt::arg("Y", tm->tm_year + 1900),
                     fmt::arg("m", tm->tm_mon + 1),
                     fmt::arg("b", CTX_IFACE_(BLT_I18NCONTEXT_SHORTMONTH, months[month_index])),
                     fmt::arg("d", tm->tm_mday),
                     fmt::arg("H", tm->tm_hour),
                     fmt::arg("M", tm->tm_min),
                     fmt::arg("I", (tm->tm_hour % 12) == 0 ? 12 : (tm->tm_hour % 12)),
                     fmt::arg("p",
                              (tm->tm_hour < 12) ? CTX_IFACE_(BLT_I18NCONTEXT_TIME, "AM") :
                                                   CTX_IFACE_(BLT_I18NCONTEXT_TIME, "PM")));
}

/* Public functions. */

std::string time(const std::tm *date_time, const char *locale_iso)
{
  const CLDRPatterns *pattern = get_locale_patterns(locale_iso);
  return format_with_pattern(date_time, pattern->time, locale_iso);
}

std::string date(const std::tm *date_time, const char *locale_iso)
{
  const CLDRPatterns *pattern = get_locale_patterns(locale_iso);
  return format_with_pattern(date_time, pattern->date, locale_iso);
}

std::string datetime(const std::tm *datetime,
                     const char *locale_iso,
                     const std::tm *now,
                     const StringRef &today,
                     const StringRef &yesterday)
{
  bool is_today = false;
  bool is_yesterday = false;
  if (now && !today.is_empty() && !yesterday.is_empty()) {
    is_today = (datetime->tm_yday == now->tm_yday && datetime->tm_year == now->tm_year);
    tm yesterday = *now;
    yesterday.tm_mday--;
    mktime(&yesterday);
    is_yesterday = (datetime->tm_yday == yesterday.tm_yday &&
                    datetime->tm_year == yesterday.tm_year);
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
