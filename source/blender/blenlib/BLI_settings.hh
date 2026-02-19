/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#pragma once

#include "..\..\..\extern\toml11\toml.hpp"

namespace blender {

extern toml::value settings_current;
extern toml::value settings_default;

void BLI_settings_init();
bool BLI_settings_save();

struct Settings {
  std::string section;

  Settings(const std::string &section) : section(section) {}

  bool exists(const std::string &item);

  template<typename T> T get(const std::string &item);

  template<typename T> void set(const std::string &item, const T &value);
};

template<typename T> T Settings::get(const std::string &item)
{
  if (settings_current.is_empty()) {
    BLI_settings_init();
  }

  try {
    return toml::get<T>(settings_current[section][item]);
  }
  catch (const toml::type_error &) {
    /* fall through to default. */
  }

  try {
    return toml::get<T>(settings_default[section][item]);
  }
  catch (const toml::type_error &) {
    /* fall through to final fallback. */
  }

  return T{};
}

template<typename T> void Settings::set(const std::string &item, const T &value)
{
  if (settings_current.is_empty()) {
    BLI_settings_init();
  }
  settings_current[section][item] = value;
}

/**********************************/

/* Default settings. */

const std::string default_settings_toml = R"_delim_(
title = "Settings"
name = "Blender"

["window.dimensions"]
userpref = [100.0, 940.0, 350.0, 900.0]
file = [100.0, 1160.0, 350.0, 950.0]
image = [50.0, 1360.0, 50.0, 830.0]
graph = [50.0, 950.0, 200.0, 780.0]
info = [100.0, 1000.0, 300.0, 880.0]
outliner = [100.0, 550.0, 350.0, 800.0]

[file_browser]
details_flags = 3
thumbnail_size = 96
filter_id = 0
display_type = 1
sort_type = 1
flag = 0

)_delim_";

}  // namespace blender
