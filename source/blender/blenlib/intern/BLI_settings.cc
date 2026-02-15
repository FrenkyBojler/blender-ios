/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 */

#include "BKE_appdir.hh"

#include "BLI_fileops.h"
#include "BLI_path_utils.hh"
#include "BLI_settings.hh" /* Own include. */

namespace blender {

constexpr toml::spec version = toml::spec::v(1, 1, 0);
#define BLI_SETTINGS_FILE_NAME "settings.toml"

toml::value data;

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
  if (BLI_exists(settings_file_path().c_str())) {
    /* Read existing settings file. */
    toml::result result = toml::try_parse(settings_file_path(), version);
    if (result.is_ok()) {
      data = result.unwrap();
    }
    else {
      bli_settings_print_errors(result.unwrap_err());
    }
  }
  else {
    /* Create a new settings file from defaults. */
    toml::result result = toml::try_parse_str(default_settings, version);
    if (result.is_ok()) {
      data = result.unwrap();
      /* Save now so this can be edited while running. */
      BLI_settings_save();
    }
    else {
      bli_settings_print_errors(result.unwrap_err());
    }
  }
}

bool BLI_settings_save()
{
  if (data.is_empty()) {
    return false;
  }
  std::string s = toml::format(data, version);
  FILE *fp = BLI_fopen(settings_file_path().c_str(), "w");
  fputs(s.c_str(), fp);
  fclose(fp);
  return true;
}

bool Settings::exists(const std::string &item)
{
  if (data.is_empty()) {
    BLI_settings_init();
  }

  const auto &section_value = toml::find(data, section);
  const auto &table = section_value.as_table();
  return table.count(item) > 0;
}

}  // namespace blender
