/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * MemoryFile / MemorySection API for persisted UI state.
 */

#pragma once

#include <string>
#include <vector>

#include "BLI_string_ref.hh"

namespace blender {

struct MemorySection {
  std::string section;

  MemorySection() = default;
  MemorySection(const StringRef sec) : section(sec.data(), sec.size()) {}
  MemorySection(const std::string &sec) : section(sec) {}
  MemorySection(const char *sec) : section(sec) {}

  template<typename T> T get(const StringRef item) const;
  template<typename T> void set(const StringRef item, const T &value);

  void remove(const StringRef item);
  void remove_section();

  struct Proxy {
    std::string *section_ptr;
    std::string key; /* Owning copy of the key */

    Proxy(std::string *section_p, const StringRef key_)
        : section_ptr(section_p), key(key_.data(), key_.size())
    {
    }

    template<typename T> operator T() const
    {
      return MemorySection(*section_ptr).get<T>(StringRef(key));
    }

    operator std::string() const
    {
      return MemorySection(*section_ptr).get<std::string>(StringRef(key));
    }

    template<typename T> Proxy &operator=(const T &value)
    {
      MemorySection(*section_ptr).set(StringRef(key), value);
      return *this;
    }

    Proxy &operator=(const std::string &s)
    {
      MemorySection(*section_ptr).set(StringRef(key), s);
      return *this;
    }

    Proxy &operator=(const char *s)
    {
      MemorySection(*section_ptr).set(StringRef(key), std::string(s));
      return *this;
    }
  };

  struct ConstProxy {
    std::string *section_ptr;
    std::string key;

    ConstProxy(std::string *section_p, const StringRef key_)
        : section_ptr(section_p), key(key_.data(), key_.size())
    {
    }

    template<typename T> operator T() const
    {
      return MemorySection(*section_ptr).get<T>(StringRef(key));
    }
    operator std::string() const
    {
      return MemorySection(*section_ptr).get<std::string>(StringRef(key));
    }
  };

  Proxy operator[](const StringRef item)
  {
    return Proxy(&section, item);
  }
  ConstProxy operator[](const StringRef item) const
  {
    return ConstProxy(const_cast<std::string *>(&section), item);
  }
};

struct MemoryFile {
  void init_async();
  void ensure_init();
  bool save() const;
  /* Open a section handle. */
  MemorySection open(const StringRef section) const
  {
    return MemorySection(section);
  }
};

/* Global instance used by callers. */
extern MemoryFile uimemory;

/**********************************/

const StringRef default_toml = R"_delim_(
title = "Saved UI State Settings"
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
