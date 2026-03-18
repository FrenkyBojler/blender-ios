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

class Section {
 public:
  explicit Section(const StringRef sec) : section_(sec.data(), sec.size()) {}

  template<typename T> T get(const StringRef item_key) const;
  template<typename T> void set(const StringRef item_key, const T &value);

  void remove(const StringRef item_key);
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

  Proxy operator[](const StringRef item_key)
  {
    return Proxy(this, item_key);
  }

 private:
  std::string section_;
};

struct Memory {
  void init();
  void init_async();
  void ensure_init() const;
  bool save() const;
  Section section(const StringRef section_name) const
  {
    return Section(section_name);
  }
};

/* Global instance used by callers. */
extern Memory memory;

}  // namespace blender::ui_memory
