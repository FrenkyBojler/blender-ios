/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * Memory / Section API for persisted UI state.
 */

#pragma once

#include <string>
#include <vector>

#include "BLI_string_ref.hh"

namespace blender::ui_memory {

struct Section {
  std::string section;

  Section(const StringRef sec) : section(sec.data(), sec.size()) {}

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
      return Section(*section_ptr).get<T>(StringRef(key));
    }

    template<typename T> Proxy &operator=(const T &value)
    {
      Section(*section_ptr).set(StringRef(key), value);
      return *this;
    }
  };

  Proxy operator[](const StringRef item)
  {
    return Proxy(&section, item);
  }
};

struct Memory {
  void init_async();
  void ensure_init();
  bool save() const;
  /* Open a section handle. */
  Section open(const StringRef section) const
  {
    return Section(section);
  }
};

/* Global instance used by callers. */
extern Memory memory;

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

["temp.window.dimensions"]
_default = [100.0, 900.0, 200.0, 800.0]
PREFERENCES = [100.0, 940.0, 350.0, 900.0]
FILE_BROWSER = [100.0, 1160.0, 350.0, 950.0]
IMAGE_EDITOR = [50.0, 1360.0, 50.0, 830.0]
GRAPH_EDITOR = [50.0, 950.0, 200.0, 780.0]
INFO = [100.0, 1000.0, 300.0, 880.0]
OUTLINER = [100.0, 550.0, 350.0, 800.0]

["panel.sortorder"]
_default = 0

["panel.open"]
_default = true

[show.dialog.info]
_default = true

[show.dialog.warnings]
_default = true

[show.dialog.confirmations]
_default = true

)_delim_";

}  // namespace blender::ui_memory
