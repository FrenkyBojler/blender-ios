/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#include <fmt/format.h>
#include <regex>
#include <string>

#include "BLI_map.hh"
#include "BLI_string_date.hh"

namespace blender::date_format {

struct CLDRPatterns {
  const char date[17];
  const char time[9];
};

static const Map<std::string, CLDRPatterns> &locale_patterns = *([]() {
  return new Map<std::string, CLDRPatterns>{
      {"default", {"%d %b %Y", "%H:%M"}},
      {"en_US", {"%b %d, %Y", "%I:%M %p"}},   /* English (US).*/
      {"en_GB", {"%d %b %Y", "%H:%M"}},       /* English (UK). */
      {"ar_EG", {"%d %b %Y", "%I:%M %p"}},    /* Arabic (Egypt). */
      {"eu_EU", {"%d %b %Y", "%H:%M"}},       /* Basque. */
      {"bg_BG", {"%d %b %Y", "%H:%M"}},       /* Bulgarian. */
      {"ca_AD", {"%d %b %Y", "%H:%M"}},       /* Catalan. */
      {"zh_HANS", {"%Y年%B%d日", "%H:%M"}},   /* Chinese (Simplified). */
      {"zh_HANT", {"%Y年%B%d日", "%H:%M"}},   /* Chinese (Traditional). */
      {"hr", {"%d %b %Y", "%H:%M"}},          /* Croatian. */
      {"cs_CZ", {"%d %b %Y", "%H:%M"}},       /* Czech. */
      {"da", {"%d %b %Y", "%H:%M"}},          /* Danish. */
      {"nl_NL", {"%d %b %Y", "%H:%M"}},       /* Dutch - Nederlands. */
      {"fi_FI", {"%d %b %Y", "%H:%M"}},       /* Finnish. */
      {"fr_FR", {"%d %b %Y", "%H:%M"}},       /* French. */
      {"ka", {"%d %b %Y", "%H:%M"}},          /* Georgian. */
      {"de_DE", {"%d %b %Y", "%H:%M"}},       /* German. */
      {"el_GR", {"%d %b %Y", "%H:%M"}},       /* Greek. */
      {"he_IL", {"%d %b %Y", "%H:%M"}},       /* Hebrew. */
      {"hi_IN", {"%d %b %Y", "%H:%M"}},       /* Hindi. */
      {"hu_HU", {"%Y. %b %d", "%H:%M"}},      /* Hungarian. */
      {"id_ID", {"%d %b %Y", "%H:%M"}},       /* Indonesian. */
      {"it_IT", {"%d %b %Y", "%H:%M"}},       /* Italian. */
      {"ja_JP", {"%Y年%b月%d日", "%H:%M"}},   /* Japanese. */
      {"ko_KR", {"%Y년 %B월%d일", "%H:%M"}},  /* Korean. */
      {"nb", {"%d %b %Y", "%H:%M"}},          /* Norwegian. */
      {"fa_IR", {"%d %b %Y", "%H:%M"}},       /* Persian. */
      {"pl_PL", {"%d %b %Y", "%H:%M"}},       /* Polish. */
      {"pt_BR", {"%d %b %Y", "%H:%M"}},       /* Portuguese (Brazil). */
      {"pt_PT", {"%d %b %Y", "%H:%M"}},       /* Portuguese (Portugal). */
      {"ro_RO", {"%d %b %Y", "%H:%M"}},       /* Romanian. */
      {"ru_RU", {"%d %b %Y", "%H:%M"}},       /* Russian. */
      {"sr_RS", {"%d %b %Y", "%H:%M"}},       /* Serbian (Cyrillic). */
      {"sr_RS@latin", {"%d %b %Y", "%H:%M"}}, /* Serbian (Latin). */
      {"sk_SK", {"%d %b %Y", "%H:%M"}},       /* Slovak. */
      {"sl", {"%d %b %Y", "%H:%M"}},          /* Slovenian. */
      {"es", {"%d %b %Y", "%H:%M"}},          /* Spanish. */
      {"sv_SE", {"%d %b %Y", "%H:%M"}},       /* Swedish. */
      {"sw", {"%d %b %Y", "%H:%M"}},          /* Swahili. */
      {"ta", {"%d %b %Y", "%H:%M"}},          /* Tamil. */
      {"th_TH", {"%d %b %Y", "%H:%M"}},       /* Thai. */
      {"tr_TR", {"%d %b %Y", "%H:%M"}},       /* Turkish. */
      {"uk_UA", {"%d %b %Y", "%H:%M"}},       /* Ukrainian. */
      {"ur", {"%d %b %Y", "%I:%M %p"}},       /* Urdu. */
      {"vi_VN", {"%d %b %Y", "%H:%M"}},       /* Vietnamese. */
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

static std::locale make_locale_fallbacks(const char *iso)
{
  if (iso && iso[0]) {
    try {
      return std::locale(iso);
    }
    catch (...) {
      try {
        std::string s = std::string(iso) + ".UTF-8";
        return std::locale(s.c_str());
      }
      catch (...) {
      }
    }
  }
  try {
    return std::locale("");
  }
  catch (...) {
    return std::locale::classic();
  }
}

static std::string format_with_pattern(const std::tm *tm,
                                       const std::string &pattern,
                                       const char *locale_iso)
{
  std::ostringstream oss;
  oss.imbue(make_locale_fallbacks(locale_iso));
  oss << std::put_time(tm, pattern.c_str());
  return oss.str();
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
  std::string out = format_with_pattern(date_time, pattern->date, locale_iso);

  /* Remove the leading zero from the day. */
  std::regex leading_zero_re(R"((^|[\s,\.])0([1-9])(\b))");
  out = std::regex_replace(out, leading_zero_re, "$1$2");
  return out;
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
