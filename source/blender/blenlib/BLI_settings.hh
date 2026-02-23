/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#pragma once

#include "BLI_string_ref.hh"

namespace blender {

void BLI_settings_init();
bool BLI_settings_save();

struct Settings {
  StringRef section;

  Settings(const StringRef &section) : section(section) {}

  template<typename T> T get(const StringRef &item) const;

  template<typename T> void set(const StringRef &item, const T &value);

  void remove(const StringRef &item);
  void remove_section();

  struct Proxy {
    StringRef section;
    StringRef key;

    Proxy(StringRef section_, StringRef key_) : section(section_), key(key_) {}

    template<typename T> operator T() const
    {
      return Settings(section).get<T>(key);
    }

    template<typename T> Proxy &operator=(const T &value)
    {
      Settings(section).set<T>(key, value);
      return *this;
    }
  };

  struct ConstProxy {
    StringRef section;
    StringRef key;
    ConstProxy(StringRef section_, StringRef key_) : section(section_), key(key_) {}

    template<typename T> operator T() const
    {
      return Settings(section).get<T>(key);
    }
  };

  Proxy operator[](const StringRef &item)
  {
    return Proxy(section, item);
  }

  ConstProxy operator[](const StringRef &item) const
  {
    return ConstProxy(section, item);
  }
};

/**********************************/

/* Default settings. */

const std::string default_settings_toml = R"_delim_(
title = "Settings"
name = "Blender"

["file_browser"]
details_flags = 3
thumbnail_size = 96
filter_id = 0
display_type = 1
sort_type = 1
flag = 0

["asset_browser"]
details_flags = 3
thumbnail_size = 96
filter_id = 0
display_type = 1
sort_type = 1
flag = 0

["file_browser.panels"]
bookmarks_index = 0
system_index = 1
volumes_index = 2
recent_index = 3
advanced_filter_index = 4
bookmarks_open = true
system_open = true
volumes_open = true
recent_open = true
advanced_filter_open = true

["window.dimensions"]
userpref = [100.0, 940.0, 350.0, 900.0]
file = [100.0, 1160.0, 350.0, 950.0]
image = [50.0, 1360.0, 50.0, 830.0]
graph = [50.0, 950.0, 200.0, 780.0]
info = [100.0, 1000.0, 300.0, 880.0]
outliner = [100.0, 550.0, 350.0, 800.0]

)_delim_";

}  // namespace blender
