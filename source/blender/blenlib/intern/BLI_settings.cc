/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 */

#include "BKE_appdir.hh"

#include "BLI_fileops.h"
#include "BLI_mutex.hh"
#include "BLI_path_utils.hh"
#include "BLI_settings.hh"

namespace blender {

constexpr toml::spec version = toml::spec::v(1, 1, 0);
#define BLI_SETTINGS_FILE_NAME "settings.toml"

toml::value settings_current;
toml::value settings_default;

static Mutex settings_mutex;

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

void BLI_settings_init()
{
  /* Load default settings. */
  toml::result result = toml::try_parse_str(default_settings_toml, version);
  if (result.is_ok()) {
    std::lock_guard<Mutex> lock(settings_mutex);
    settings_default = result.unwrap();
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
      settings_current = result.unwrap();
    }
    else {
      bli_settings_print_errors(result.unwrap_err());
    }
  }
  else {
    /* Create a new settings file from defaults. */
    {
      std::lock_guard<Mutex> lock(settings_mutex);
      settings_current = settings_default;
    }
    settings_current = settings_default;
    BLI_settings_save();
  }
}

bool BLI_settings_save()
{
  std::lock_guard<Mutex> lock(settings_mutex);
  if (settings_current.is_empty()) {
    return false;
  }
  std::string s = toml::format(settings_current, version);
  FILE *fp = BLI_fopen(settings_file_path().c_str(), "w");
  fputs(s.c_str(), fp);
  fclose(fp);
  return true;
}

void Settings::remove(const StringRef &item)
{
  std::lock_guard<Mutex> lock(settings_mutex);
  if (settings_current.is_empty()) {
    BLI_settings_init();
  }

  /* Ensure the root is a table and the section exists as a table. */
  if (!settings_current.is_table()) {
    return;
  }

  auto &root_tbl = settings_current.as_table();
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

}  // namespace blender
