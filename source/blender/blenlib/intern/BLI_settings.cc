/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * A min-heap / priority queue ADT.
 *
 * Simplified version of the heap that only supports insertion and removal from top.
 *
 * See BLI_heap.c for a more full featured heap implementation.
 */

#include "BKE_appdir.hh"

#include "BLI_fileops.h"
#include "BLI_path_utils.hh"
#include "BLI_settings.hh" /* Own include. */

namespace blender {

constexpr toml::spec version = toml::spec::v(1, 1, 0);
#define BLI_SETTINGS_FILE_NAME "settings.toml"
static toml::value settings;

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
      settings = result.unwrap();
    }
    else {
      bli_settings_print_errors(result.unwrap_err());
    }
  }
  else {
    /* Create a new settings file from defaults.*/
    toml::result result = toml::try_parse_str(default_settings, version);
    if (result.is_ok()) {
      settings = result.unwrap();
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
  if (settings.is_empty()) {
    return false;
  }

  std::string s = toml::format(settings, version);
  FILE *fp = BLI_fopen(settings_file_path().c_str(), "w");
  fputs(s.c_str(), fp);
  fclose(fp);
  return true;
}

std::string BLI_settings_get_string(std::string table, std::string key, std::string default_value)
{
  if (settings.is_empty()) {
    BLI_settings_init();
  }
  return toml::get_or(settings[table][key], default_value);
}

void BLI_settings_set_string(std::string table, std::string key, std::string value)
{
  if (settings.is_empty()) {
    BLI_settings_init();
  }
  settings[table][key] = value;
}

char BLI_settings_get_char(std::string table, std::string key, char default_value)
{
  if (settings.is_empty()) {
    BLI_settings_init();
  }
  return toml::get_or(settings[table][key], default_value);
}

void BLI_settings_set_char(std::string table, std::string key, char value)
{
  if (settings.is_empty()) {
    BLI_settings_init();
  }
  settings[table][key] = value;
}

int32_t BLI_settings_get_int(std::string table, std::string key, int32_t default_value)
{
  if (settings.is_empty()) {
    BLI_settings_init();
  }
  return toml::get_or(settings[table][key], default_value);
}

void BLI_settings_set_int(std::string table, std::string key, int32_t value)
{
  if (settings.is_empty()) {
    BLI_settings_init();
  }
  settings[table][key] = value;
}

int64_t BLI_settings_get_int64(std::string table,
                               std::string key,
                               int64_t default_value)
{
  if (settings.is_empty()) {
    BLI_settings_init();
  }
  return toml::get_or(settings[table][key], default_value);
}

void BLI_settings_set_int64(std::string table, std::string key, int64_t value)
{
  if (settings.is_empty()) {
    BLI_settings_init();
  }
  settings[table][key] = value;
}

bool BLI_settings_get_bool(std::string table, std::string key, bool default_value)
{
  if (settings.is_empty()) {
    BLI_settings_init();
  }
  return toml::get_or(settings[table][key], default_value);
}

void BLI_settings_set_bool(std::string table, std::string key, bool value)
{
  if (settings.is_empty()) {
    BLI_settings_init();
  }
  settings[table][key] = value;
}

float BLI_settings_get_float(std::string table, std::string key, float default_value)
{
  if (settings.is_empty()) {
    BLI_settings_init();
  }
  return toml::get_or(settings[table][key], default_value);
}

void BLI_settings_set_float(std::string table, std::string key, float value)
{
  if (settings.is_empty()) {
    BLI_settings_init();
  }
  settings[table][key] = value;
}

std::vector<float> BLI_settings_get_floats(std::string table, std::string key)
{
  if (settings.is_empty()) {
    BLI_settings_init();
  }
  return toml::get_or(settings[table][key], std::vector<float>{});
}

void BLI_settings_set_floats(std::string table, std::string key, std::vector<float> values)
{
  if (settings.is_empty()) {
    BLI_settings_init();
  }
  settings[table][key] = values;
}

/* -------------------------------------------------------------------- */
/** \name HeapSimple Internal Structs
 * \{ */


}  // namespace blender
