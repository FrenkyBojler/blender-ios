/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * Implementation of Memory / Section backed by TOML.
 */

#include "toml.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "BKE_appdir.hh"
#include "BKE_global.hh"
#include "BKE_uimemory.hh"

#include "BLI_fileops.h"
#include "BLI_mutex.hh"
#include "BLI_path_utils.hh"
#include "BLI_time.h"

namespace blender::ui_memory {

constexpr toml::spec version = toml::spec::v(1, 1, 0);

/* TOML storage. Protected by memory_mutex. */
static toml::value uimemory_current;
static toml::value uimemory_default;
static Mutex uimemory_mutex;      /* protects TOML values */
static Mutex uimemory_init_mutex; /* used with cv for init wait */
static std::atomic<bool> uimemory_ready{false};
static std::condition_variable_any uimemory_init_cv;
static std::once_flag uimemory_init_once;

extern const StringRef default_toml;

static std::string uimemory_file_path()
{
  std::optional<std::string> datafiles_path = BKE_appdir_folder_id(BLENDER_USER_CONFIG, "");
  if (datafiles_path.has_value()) {
    return *datafiles_path + SEP + BLENDER_RECENTS_FILE;
  }
  return {};
}

static void uimemory_print_errors(std::vector<toml::error_info> errors)
{
  for (const toml::error_info &error : errors) {
    std::string msg = toml::format_error(error);
    fprintf(stderr, "%s\n", msg.c_str());
  }
}

static void uimemory_init_impl()
{
  toml::result default_result = toml::try_parse_str(default_toml, version);
  if (default_result.is_ok()) {
    std::lock_guard<Mutex> lock(uimemory_mutex);
    uimemory_default = default_result.unwrap();
  }
  else {
    uimemory_print_errors(default_result.unwrap_err());
  }

  /* Load from on-disk file if found. */
  /* In background mode avoid any file I/O or console error output. The
   * in-memory defaults are still parsed above so API calls will work. */
  if (!G.background) {
    const std::string path = uimemory_file_path();
    if (!path.empty() && BLI_exists(path.c_str())) {
      toml::result file_result = toml::try_parse(path, version);
      if (file_result.is_ok()) {
        std::lock_guard<Mutex> lock(uimemory_mutex);
        uimemory_current = file_result.unwrap();
      }
      else {
        uimemory_print_errors(file_result.unwrap_err());
      }
    }
  }

  /* Mark ready and wake any waiters. */
  uimemory_ready.store(true, std::memory_order_release);
  uimemory_init_cv.notify_all();
}

void Memory::init()
{
  uimemory_init_impl();
}

void Memory::init_async()
{
  std::call_once(uimemory_init_once,
                 []() { std::thread([]() { uimemory_init_impl(); }).detach(); });
}

void Memory::ensure_init() const
{
  if (uimemory_ready.load(std::memory_order_acquire)) {
    return;
  }

  /* If async init wasn't started for some reason (like background mode), start
   * init synchronously. If async init is already scheduled then this call_once
   * will do nothing and we will wait below for the background thread to finish. */
  std::call_once(uimemory_init_once, uimemory_init_impl);

  if (uimemory_ready.load(std::memory_order_acquire)) {
    return;
  }

  /* Wait using BLI Mutex + condition_variable_any. */
  std::unique_lock<Mutex> lock(uimemory_init_mutex);
  uimemory_init_cv.wait(lock, [] { return uimemory_ready.load(std::memory_order_acquire); });
}

bool Memory::save() const
{
  ensure_init();

  /* Don't write files when running in background/headless mode. */
  if (G.background) {
    return false;
  }

  std::lock_guard<Mutex> lock(uimemory_mutex);
  if (uimemory_current.is_empty()) {
    return false;
  }
  std::string toml_as_string = toml::format(uimemory_current, version);
  FILE *file_handle = BLI_fopen(uimemory_file_path().c_str(), "w");
  if (file_handle == nullptr) {
    return false;
  }
  fputs(toml_as_string.c_str(), file_handle);
  fclose(file_handle);
  return true;
}

static const toml::value *uimemory_find_in(const toml::value &root,
                                           const std::string &section_name,
                                           const std::string &item_key)
{
  if (!root.is_table()) {
    /* Only tables can contain key/value pairs. */
    return nullptr;
  }

  const toml::table &root_table = root.as_table();

  if (section_name.empty()) {
    /* No section name, so look in the root of the document. */
    toml::table::const_iterator root_iter = root_table.find(item_key);
    if (root_iter != root_table.end()) {
      /* std::pair. first is key, second is value. */
      return &root_iter->second;
    }
    return nullptr;
  }

  toml::table::const_iterator section_iter = root_table.find(section_name);
  if (section_iter == root_table.end()) {
    /* The named section does not exist. */
    return nullptr;
  }
  const toml::value &section_value = section_iter->second;
  if (!section_value.is_table()) {
    /* The named section is not a table. */
    return nullptr;
  }

  /* Search the named section table for the item key value. */
  const toml::table &section_table = section_value.as_table();
  toml::table::const_iterator result = section_table.find(item_key);
  if (result != section_table.end()) {
    return &result->second;
  }

  return nullptr;
}

template<typename T> T Section::get(const StringRef item_key) const
{
  memory.ensure_init();

  std::lock_guard<Mutex> lock(uimemory_mutex);

  /* Try user value first. */
  if (const toml::value *v = uimemory_find_in(uimemory_current, section_, item_key)) {
    return toml::get_or(*v, T{});
  }

  /* Per-key default value. */
  if (const toml::value *v = uimemory_find_in(uimemory_default, section_, item_key)) {
    return toml::get_or(*v, T{});
  }

  /* User's section-level "_default" (user override of section default). */
  if (!section_.empty()) {
    if (const toml::value *v = uimemory_find_in(uimemory_current, section_, "_default")) {
      return toml::get_or(*v, T{});
    }
  }

  /* Builtin section-level "_default". */
  if (!section_.empty()) {
    if (const toml::value *v = uimemory_find_in(uimemory_default, section_, "_default")) {
      return toml::get_or(*v, T{});
    }
  }

  /* Final fallback. False, 0, 0,0f, {}, etc. */
  return T{};
}

template<typename T> void Section::set(const StringRef item_key, const T &value)
{
  memory.ensure_init();

  std::lock_guard<Mutex> lock(uimemory_mutex);
  if (section_.empty()) {
    uimemory_current[item_key] = value;
  }
  else {
    uimemory_current[section_][item_key] = value;
  }
}

void Section::remove(const StringRef item_key)
{
  memory.ensure_init();
  std::lock_guard<Mutex> lock(uimemory_mutex);
  if (!uimemory_current.is_table()) {
    return;
  }
  toml::table &root_table = uimemory_current.as_table();
  if (section_.empty()) {
    root_table.erase(item_key);
    return;
  }
  toml::table::iterator section_iter = root_table.find(section_);
  if (section_iter == root_table.end()) {
    return;
  }
  toml::value &section_value = section_iter->second;
  if (!section_value.is_table()) {
    return;
  }
  toml::table &section_table = section_value.as_table();
  section_table.erase(item_key);
}

void Section::remove_section()
{
  memory.ensure_init();
  std::lock_guard<Mutex> lock(uimemory_mutex);
  if (section_.empty() || !uimemory_current.is_table()) {
    return;
  }
  toml::table &root_table = uimemory_current.as_table();
  root_table.erase(section_);
}

template std::string Section::get<std::string>(const StringRef item_key) const;
template void Section::set<std::string>(const StringRef item_key, const std::string &value);

template char Section::get<char>(const StringRef item_key) const;
template void Section::set<char>(const StringRef item_key, const char &value);
template bool Section::get<bool>(const StringRef item_key) const;
template void Section::set<bool>(const StringRef item_key, const bool &value);

template int16_t Section::get<int16_t>(const StringRef item_key) const;
template void Section::set<int16_t>(const StringRef item_key, const int16_t &value);

template uint16_t Section::get<uint16_t>(const StringRef item_key) const;
template void Section::set<uint16_t>(const StringRef item_key, const uint16_t &value);

template int32_t Section::get<int32_t>(const StringRef item_key) const;
template void Section::set<int32_t>(const StringRef item_key, const int32_t &value);

template uint32_t Section::get<uint32_t>(const StringRef item_key) const;
template void Section::set<uint32_t>(const StringRef item_key, const uint32_t &value);

template int64_t Section::get<int64_t>(const StringRef item_key) const;
template void Section::set<int64_t>(const StringRef item_key, const int64_t &value);

template uint64_t Section::get<uint64_t>(const StringRef item_key) const;
template void Section::set<uint64_t>(const StringRef item_key, const uint64_t &value);

template float Section::get<float>(const StringRef item_key) const;
template void Section::set<float>(const StringRef item_key, const float &value);

template double Section::get<double>(const StringRef item_key) const;
template void Section::set<double>(const StringRef item_key, const double &value);

template std::vector<int> Section::get<std::vector<int>>(const StringRef item_key) const;
template void Section::set<std::vector<int>>(const StringRef item_key,
                                             const std::vector<int> &value);

template std::vector<double> Section::get<std::vector<double>>(const StringRef item_key) const;
template void Section::set<std::vector<double>>(const StringRef item_key,
                                                const std::vector<double> &value);

template std::vector<float> Section::get<std::vector<float>>(const StringRef item_key) const;
template void Section::set<std::vector<float>>(const StringRef item_key,
                                               const std::vector<float> &value);

/* Global instance. */
Memory memory;

/* Defaults. */

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
details_flags = 0
thumbnail_size = 64
list_thumbnail_size = 32
list_column_size = 220
filter_id = 0
display_type = 3
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
_default = -1

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
