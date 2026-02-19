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

  template<typename T> T get(const std::string &item) const;

  template<typename T> void set(const std::string &item, const T &value);

  struct Proxy {
    std::string section;
    std::string key;

    Proxy(std::string section_, std::string key_)
        : section(std::move(section_)), key(std::move(key_))
    {
    }

    template<typename T> operator T() const
    {
      return Settings(section).get<T>(key);
    }

    template<typename T> Proxy &operator=(const T &value)
    {
      Settings(section).set<T>(key, value);
      return *this;
    }

    /* Allow assigning toml::value directly for advanced use. */
    Proxy &operator=(const toml::value &v)
    {
      Settings(section).set<toml::value>(key, v);
      return *this;
    }
  };

  struct ConstProxy {
    std::string section;
    std::string key;

    ConstProxy(std::string section_, std::string key_)
        : section(std::move(section_)), key(std::move(key_))
    {
    }

    template<typename T> operator T() const
    {
      return Settings(section).get<T>(key);
    }
  };

  /* Return proxies that own the section string (cheap copy). */
  Proxy operator[](const std::string &item)
  {
    return Proxy(section, item);
  }

  ConstProxy operator[](const std::string &item) const
  {
    return ConstProxy(section, item);
  }
};

template<typename T> T Settings::get(const std::string &item) const
{
  if (settings_current.is_empty()) {
    BLI_settings_init();
  }
  const toml::value &cur = settings_current[section][item];
  const toml::value &def = settings_default[section][item];
  return toml::get_or(cur, toml::get_or(def, T{}));
}

template<typename T> void Settings::set(const std::string &item, const T &value)
{
  if (settings_current.is_empty()) {
    BLI_settings_init();
  }
  settings_current[section][item] = value;
}

/* Global, ergonomic access: settings["Section"]["item"] */
struct SettingsRoot {
  struct ValueProxy {
    std::string section;
    std::string key;

    ValueProxy(std::string section_, std::string key_)
        : section(std::move(section_)), key(std::move(key_))
    {
    }

    template<typename T> operator T() const
    {
      return Settings(section).get<T>(key);
    }

    template<typename T> ValueProxy &operator=(const T &value)
    {
      Settings(section).set<T>(key, value);
      return *this;
    }

    /* Allow assignment from toml::value too (advanced use). */
    ValueProxy &operator=(const toml::value &v)
    {
      Settings(section).set<toml::value>(key, v);
      return *this;
    }
  };

  struct SectionProxy {
    std::string section;
    SectionProxy(std::string s) : section(std::move(s)) {}
    ValueProxy operator[](const std::string &key) const
    {
      return ValueProxy(section, key);
    }
  };

  SectionProxy operator[](const std::string &section) const
  {
    return SectionProxy(section);
  }
};

/* Global instance that can be used everywhere: settings["file_browser"]["width"]. */
extern const SettingsRoot settings;

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
