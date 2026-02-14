/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#pragma once

#include "..\..\..\extern\toml11\toml.hpp"

namespace blender {

extern toml::value data;

struct settings_t {

  void init();
  bool save();

  template<typename T>
  T get(const std::string &section, const std::string &item, const T &default_value);

  template<typename T>
  void set(const std::string &section, const std::string &item, const T &value);
};

static settings_t settings;

template<typename T>
T settings_t::get(const std::string &section, const std::string &item, const T &default_value)
{
  if (data.is_empty()) {
    init();
  }
  return toml::get_or(data[section][item], default_value);
}

template<typename T>
void settings_t::set(const std::string &section, const std::string &item, const T &value)
{
  if (data.is_empty()) {
    init();
  }
  data[section][item] = value;
}

/**********************************/

/* Default settings file content. */

const std::string default_settings = R"_delim_(
title = "Settings"
name = "Blender"

["window.dimensions"]
userpref = [100.0, 940.0, 350.0, 900.0]
file = [100.0, 1160.0, 350.0, 950.0]
image = [50.0, 1360.0, 50.0, 830.0]
graph = [50.0, 950.0, 200.0, 780.0]
info = [100.0, 1000.0, 300.0, 880.0]
outliner = [100.0, 550.0, 350.0, 800.0]

)_delim_";

}  // namespace blender
