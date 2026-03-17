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
    Section *section_ptr;
    std::string key; /* Owning copy of the key */

    Proxy(Section *section_p, const StringRef key_)
        : section_ptr(section_p), key(key_.data(), key_.size())
    {
    }

    template<typename T> operator T() const
    {
      return this->section_ptr->get<T>(StringRef(this->key));
    }

    template<typename T> Proxy &operator=(const T &value)
    {
      this->section_ptr->set(StringRef(this->key), value);
      return *this;
    }
  };

  Proxy operator[](const StringRef item)
  {
    return Proxy(this, item);
  }
};

struct Memory {
  void init();
  void init_async();
  void ensure_init() const;
  bool save() const;
  /* Open a section handle. */
  Section open(const StringRef section) const
  {
    return Section(section);
  }
};

/* Global instance used by callers. */
extern Memory memory;

}  // namespace blender::ui_memory
