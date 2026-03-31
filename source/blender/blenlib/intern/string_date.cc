/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 * \brief Hmmm...
 *
 * This..
 */

#include <string>
#include <unordered_map>
#include <regex>

#include <fmt/format.h>

#include "BLI_string_date.hh"

namespace blender {

struct CLDRLocalePatterns {
  const char date[3][14];
  const char time[3][12];
  const char datetime[8];
};

static const std::unordered_map<std::string, CLDRLocalePatterns> cldr_locale_table = {
    {"default", {{"%d/%m/%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"en_US", /* English (US).*/
     {{"%d/%m/%y", "%b %d, %Y", "%B %d, %Y"}, {"%I:%M %p", "%I:%M %p", "%I:%M:%S %p"}, "{1} {0}"}},
    {"en_GB", /* English (UK). */
     {{"%d/%m/%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"ar_EG", /* Arabic (Egypt). */
     {{"%d/%m/%y", "%d %b %Y", "%d %B %Y"}, {"%I:%M %p", "%I:%M %p", "%I:%M:%S %p"}, "{1} {0}"}},
    {"eu_EU", /* Basque. */
     {{"%d/%m/%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"bg_BG", /* Bulgarian. */
     {{"%d.%m.%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"ca_AD", /* Catalan. */
     {{"%d/%m/%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"zh_HANS", /* Chinese (Simplified). */
     {{"%y/%m/%d", "%Y年%b%d日", "%Y年%B%d日"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"zh_HANT", /* Chinese (Traditional). */
     {{"%y/%m/%d", "%Y年%b%d日", "%Y年%B%d日"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"hr", /* :Croatian. */
     {{"%d.%m.%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"cs_CZ", /* Czech. */
     {{"%d.%m.%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"da", /* Danish. */
     {{"%d-%m-%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"nl_NL", /* Dutch - Nederlands. */
     {{"%d-%m-%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"fi_FI", /* Finnish. */
     {{"%d.%m.%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"fr_FR", /* French. */
     {{"%d/%m/%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"ka", /* Georgian. */
     {{"%d.%m.%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"de_DE", /* German. */
     {{"%d.%m.%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"el_GR", /* Greek. */
     {{"%d/%m/%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"he_IL", /* Hebrew. */
     {{"%d/%m/%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"hi_IN", /* Hindi. */
     {{"%d/%m/%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"hu_HU", /* Hungarian. */
     {{"%y.%m.%d", "%Y. %b %d", "%Y. %B %d"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"id_ID", /* Indonesian. */
     {{"%d/%m/%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"it_IT", /* Italian. */
     {{"%d/%m/%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"ja_JP", /* Japanese. */
     {{"%y/%m/%d", "%Y年%b%d日", "%Y年%B%d日"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"ko_KR", /* Korean. */
     {{"%y. %m. %d.", "%Y년 %b%d일", "%Y년 %B%d일"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"nb", /* Norwegian. */
     {{"%d.%m.%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"fa_IR", /* Persian. */
     {{"%y/%m/%d", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"pl_PL", /* Polish. */
     {{"%d.%m.%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"pt_BR", /* Portuguese (Brazil). */
     {{"%d/%m/%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"pt_PT", /* Portuguese (Portugal). */
     {{"%d/%m/%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"ro_RO", /* Romanian. */
     {{"%d.%m.%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"ru_RU", /* Russian. */
     {{"%d.%m.%y", "%d %b %Y", "%d %B %Y г."}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"sr_RS", /* Serbian (Cyrillic). */
     {{"%d.%m.%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"sr_RS@latin", /* Serbian (Latin). */
     {{"%d.%m.%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"sk_SK", /* Slovak. */
     {{"%d.%m.%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"sl", /* Slovenian. */
     {{"%d.%m.%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"es", /* Spanish. */
     {{"%d/%m/%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"sv_SE", /* Swedish. */
     {{"%y-%m-%d", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"sw", /* Swahili. */
     {{"%d/%m/%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"ta", /* Tamil. */
     {{"%d/%m/%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"th_TH", /* Thai. */
     {{"%d/%m/%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"tr_TR", /* Turkish. */
     {{"%d.%m.%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"uk_UA", /* Ukrainian. */
     {{"%d.%m.%y", "%d %b %Y", "%d %B %Y р."}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
    {"ur", /* Urdu. */
     {{"%d/%m/%y", "%d %b %Y", "%d %B %Y"}, {"%I:%M %p", "%I:%M %p", "%I:%M:%S %p"}, "{1} {0}"}},
    {"vi_VN", /* Vietnamese. */
     {{"%d/%m/%y", "%d %b %Y", "%d %B %Y"}, {"%H:%M", "%H:%M", "%H:%M:%S"}, "{1} {0}"}},
};

/* Helper: get patterns for a locale (exact key, then language prefix, then default). */
static const CLDRLocalePatterns &get_locale_patterns(const char *locale_iso)
{
  static const CLDRLocalePatterns default_patterns = cldr_locale_table.at("default");

  const char *loc_in = locale_iso;
  if (loc_in == nullptr || loc_in[0] == '\0') {
    return default_patterns;
  }

  std::string loc(loc_in);
  auto it = cldr_locale_table.find(loc);
  if (it != cldr_locale_table.end()) {
    return it->second;
  }

  if (loc.size() >= 2) {
    std::string lang = loc.substr(0, 2);
    it = cldr_locale_table.find(lang);
    if (it != cldr_locale_table.end()) {
      return it->second;
    }
  }

  return default_patterns;
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

std::string BLI_date_format_time(const std::tm *date_time,
                                 BLI_DateFormatStyle style,
                                 const char *locale_iso)
{
  const auto &pat = get_locale_patterns(locale_iso);
  const std::string &pattern = pat.time[static_cast<int>(style)];
  return format_with_pattern(date_time, pattern, locale_iso);
}

std::string BLI_date_format_date(const std::tm *date_time,
                                 BLI_DateFormatStyle style,
                                 const char *locale_iso)
{
  const auto &pat = get_locale_patterns(locale_iso);
  const std::string &pattern = pat.date[static_cast<int>(style)];
  std::string out = format_with_pattern(date_time, pattern, locale_iso);

  /* Remove the leading zero from the day. */
  if (style == BLI_DateFormatStyle::Medium || style == BLI_DateFormatStyle::Long) {
    std::regex leading_zero_re(R"((^|[\s,\.])0([1-9])(\b))");
    out = std::regex_replace(out, leading_zero_re, "$1$2");
  }
  return out;
}

std::string BLI_date_format_datetime(const std::tm *datetime,
                                     BLI_DateFormatStyle style,
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

  const std::string time_s = BLI_date_format_time(datetime, style, locale_iso);

  if (is_today) {
    return std::string(today) + " " + time_s;
  }
  else if (is_yesterday) {
    return std::string(yesterday) + " " + time_s;
  }
  else {
    std::string datetime_s;
    const std::string date_s = BLI_date_format_date(datetime, style, locale_iso);
    const auto &pat = get_locale_patterns(locale_iso);
    datetime_s = fmt::format(
        fmt::runtime(pat.datetime), fmt::arg("0", time_s), fmt::arg("1", date_s));
    return datetime_s;
  }
}

}  // namespace blender
