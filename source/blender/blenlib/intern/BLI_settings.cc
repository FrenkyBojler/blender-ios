/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 */

#include "../../../extern/toml11/toml.hpp"

#include <mutex> /* std::once_flag, std::call_once */

#include "BKE_appdir.hh"

#include "BLI_fileops.h"
#include "BLI_mutex.hh"
#include "BLI_path_utils.hh"
#include "BLI_settings.hh"

namespace blender {

constexpr toml::spec version = toml::spec::v(1, 1, 0);
#define BLI_SETTINGS_FILE_NAME "settings.toml"

/* Lazy (first-use) storage for toml values.. */
static toml::value &BLI_settings_current()
{
  static toml::value v;
  return v;
}
static toml::value &BLI_settings_default()
{
  static toml::value v;
  return v;
}

static Mutex settings_mutex;

/* Ensure bli_settings_init() runs once, thread-safely, on first use. */
static std::once_flag settings_init_once;

static std::string settings_file_path()
{
  std::optional<std::string> datafiles_path = BKE_appdir_folder_id(BLENDER_USER_CONFIG, "");
  if (datafiles_path.has_value()) {
    return *datafiles_path + SEP + BLI_SETTINGS_FILE_NAME;
  }
  return {};
}

static void bli_settings_print_errors(std::vector<toml::error_info> errors)
{
  for (auto error : errors) {
    std::string msg = toml::format_error(error);
    printf("%s\n", msg.c_str());
  }
}

static void bli_settings_init()
{
  /* Load default settings. */
  toml::result result = toml::try_parse_str(default_settings_toml, version);
  if (result.is_ok()) {
    std::lock_guard<Mutex> lock(settings_mutex);
    BLI_settings_default() = result.unwrap();
  }
  else {
    bli_settings_print_errors(result.unwrap_err());
  }

  /* Load settings from on-disk file if found. */
  if (BLI_exists(settings_file_path().c_str())) {
    /* Read existing settings file. */
    toml::result result = toml::try_parse(settings_file_path(), version);
    if (result.is_ok()) {
      std::lock_guard<Mutex> lock(settings_mutex);
      BLI_settings_current() = result.unwrap();
    }
    else {
      bli_settings_print_errors(result.unwrap_err());
    }
  }
  else {
    /* Create a new settings file from defaults. */
    {
      std::lock_guard<Mutex> lock(settings_mutex);
      BLI_settings_current() = BLI_settings_default();
    }
    BLI_settings_save();
  }
}

bool BLI_settings_save()
{
  std::lock_guard<Mutex> lock(settings_mutex);
  if (BLI_settings_current().is_empty()) {
    return false;
  }
  std::string s = toml::format(BLI_settings_current(), version);
  FILE *fp = BLI_fopen(settings_file_path().c_str(), "w");
  fputs(s.c_str(), fp);
  fclose(fp);
  return true;
}

void Settings::remove(const StringRef item)
{
  std::call_once(settings_init_once, bli_settings_init);
  std::lock_guard<Mutex> lock(settings_mutex);

  if (!BLI_settings_current().is_table()) {
    return;
  }

  toml::table &root_tbl = BLI_settings_current().as_table();

  if (section.is_empty()) {
    root_tbl.erase(item);
    return;
  }

  auto section_it = root_tbl.find(section);
  if (section_it == root_tbl.end()) {
    return;
  }

  toml::value &sec_val = section_it->second;
  if (!sec_val.is_table()) {
    return;
  }

  toml::table &sec_tbl = sec_val.as_table();
  sec_tbl.erase(item);
}

void Settings::remove_section()
{
  std::call_once(settings_init_once, bli_settings_init);
  std::lock_guard<Mutex> lock(settings_mutex);
  if (section.is_empty() || !BLI_settings_current().is_table()) {
    return;
  }
  toml::table &root_tbl = BLI_settings_current().as_table();
  root_tbl.erase(section);
}

template<typename T> T Settings::get(const StringRef item) const
{
  std::call_once(settings_init_once, bli_settings_init);
  std::lock_guard<Mutex> lock(settings_mutex);
  const std::string sec = section;
  const std::string key = item;
  const toml::value &cur = sec.empty() ? BLI_settings_current()[key] :
                                         BLI_settings_current()[sec][key];
  const toml::value &def = sec.empty() ? BLI_settings_default()[key] :
                                         BLI_settings_default()[sec][key];
  return toml::get_or(cur, toml::get_or(def, T{}));
}

template<typename T> void Settings::set(const StringRef item, const T &value)
{
  std::call_once(settings_init_once, bli_settings_init);
  std::lock_guard<Mutex> lock(settings_mutex);
  if (section.is_empty()) {
    BLI_settings_current()[item] = value;
  }
  else {
    BLI_settings_current()[section][item] = value;
  }
}

template std::string Settings::get<std::string>(const StringRef item) const;
template void Settings::set<std::string>(const StringRef item, const std::string &value);

template char Settings::get<char>(const StringRef item) const;
template void Settings::set<char>(const StringRef item, const char &value);

template bool Settings::get<bool>(const StringRef item) const;
template void Settings::set<bool>(const StringRef item, const bool &value);

template int16_t Settings::get<int16_t>(const StringRef item) const;
template void Settings::set<int16_t>(const StringRef item, const int16_t &value);

template uint16_t Settings::get<uint16_t>(const StringRef item) const;
template void Settings::set<uint16_t>(const StringRef item, const uint16_t &value);

template int32_t Settings::get<int32_t>(const StringRef item) const;
template void Settings::set<int32_t>(const StringRef item, const int32_t &value);

template uint32_t Settings::get<uint32_t>(const StringRef item) const;
template void Settings::set<uint32_t>(const StringRef item, const uint32_t &value);

template int64_t Settings::get<int64_t>(const StringRef item) const;
template void Settings::set<int64_t>(const StringRef item, const int64_t &value);

template uint64_t Settings::get<uint64_t>(const StringRef item) const;
template void Settings::set<uint64_t>(const StringRef item, const uint64_t &value);

template float Settings::get<float>(const StringRef item) const;
template void Settings::set<float>(const StringRef item, const float &value);

template double Settings::get<double>(const StringRef item) const;
template void Settings::set<double>(const StringRef item, const double &value);

template std::string Settings::get<std::string>(const StringRef item) const;
template void Settings::set<std::string>(const StringRef item, const std::string &value);

template std::vector<int> Settings::get<std::vector<int>>(const StringRef item) const;
template void Settings::set<std::vector<int>>(const StringRef item,
                                              const std::vector<int> &value);

template std::vector<double> Settings::get<std::vector<double>>(const StringRef item) const;
template void Settings::set<std::vector<double>>(const StringRef item,
                                                 const std::vector<double> &value);

template std::vector<float> Settings::get<std::vector<float>>(const StringRef item) const;
template void Settings::set<std::vector<float>>(const StringRef item,
                                                const std::vector<float> &value);

}  // namespace blender
