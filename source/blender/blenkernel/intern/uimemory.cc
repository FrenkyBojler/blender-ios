/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * Implementation of Memory / Section backed by TOML.
 */

#include "../../../extern/toml11/toml.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "BKE_appdir.hh"
#include "BKE_uimemory.hh"

#include "BLI_fileops.h"
#include "BLI_mutex.hh"
#include "BLI_path_utils.hh"
#include "BLI_time.h"

namespace blender::ui_memory {

constexpr toml::spec version = toml::spec::v(1, 1, 0);
#define UIMEMORY_FILE_NAME "uimemory.toml"

/* TOML storage. Protected by memory_mutex. */
static toml::value uimemory_current;
static toml::value uimemory_default;
static Mutex uimemory_mutex;      /* protects TOML values */
static Mutex uimemory_init_mutex; /* used with cv for init wait */
static std::atomic<bool> uimemory_ready{false};
static std::condition_variable_any uimemory_init_cv;
static std::once_flag uimemory_init_once;

static std::string uimemory_file_path()
{
  std::optional<std::string> datafiles_path = BKE_appdir_folder_id(BLENDER_USER_CONFIG, "");
  if (datafiles_path.has_value()) {
    return *datafiles_path + SEP + UIMEMORY_FILE_NAME;
  }
  return {};
}

static void uimemory_print_errors(std::vector<toml::error_info> errors)
{
  for (auto &error : errors) {
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

  /* Load from user file if found. */
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

  std::lock_guard<Mutex> lock(uimemory_mutex);
  if (uimemory_current.is_empty()) {
    return false;
  }
  std::string s = toml::format(uimemory_current, version);
  FILE *fp = BLI_fopen(uimemory_file_path().c_str(), "w");
  if (fp == nullptr) {
    return false;
  }
  fputs(s.c_str(), fp);
  fclose(fp);
  return true;
}

static const toml::value *uimemory_find_in(const toml::value &root,
                                           const std::string &section_name,
                                           const std::string &k)
{
  if (!root.is_table()) {
    return nullptr;
  }
  const toml::table &root_tbl = root.as_table();

  if (section_name.empty()) {
    auto it = root_tbl.find(k);
    if (it != root_tbl.end()) {
      return &it->second;
    }
    return nullptr;
  }

  auto sit = root_tbl.find(section_name);
  if (sit == root_tbl.end()) {
    return nullptr;
  }
  const toml::value &sec_val = sit->second;
  if (!sec_val.is_table()) {
    return nullptr;
  }
  const toml::table &sec_tbl = sec_val.as_table();
  auto it = sec_tbl.find(k);
  if (it != sec_tbl.end()) {
    return &it->second;
  }
  return nullptr;
}

template<typename T> T Section::get(const StringRef item) const
{
  memory.ensure_init();

  std::lock_guard<Mutex> lock(uimemory_mutex);
  const std::string &sec = section;
  const std::string key(item.data(), item.size());

  /* Try user value first. */
  if (const toml::value *v = uimemory_find_in(uimemory_current, sec, key)) {
    return toml::get_or(*v, T{});
  }

  /* Per-key default value. */
  if (const toml::value *v = uimemory_find_in(uimemory_default, sec, key)) {
    return toml::get_or(*v, T{});
  }

  /* User's section-level "_default" (user override of section default). */
  if (!sec.empty()) {
    if (const toml::value *v = uimemory_find_in(uimemory_current, sec, "_default")) {
      return toml::get_or(*v, T{});
    }
  }

  /* Builtin section-level "_default". */
  if (!sec.empty()) {
    if (const toml::value *v = uimemory_find_in(uimemory_default, sec, "_default")) {
      return toml::get_or(*v, T{});
    }
  }

  /* Final fallback. False, 0, 0,0f, {}, etc. */
  return T{};
}

template<typename T> void Section::set(const StringRef item, const T &value)
{
  memory.ensure_init();

  std::lock_guard<Mutex> lock(uimemory_mutex);
  const std::string &sec = section;
  const std::string key(item.data(), item.size());
  if (sec.empty()) {
    uimemory_current[key] = value;
  }
  else {
    uimemory_current[sec][key] = value;
  }
}

void Section::remove(const StringRef item)
{
  memory.ensure_init();
  std::lock_guard<Mutex> lock(uimemory_mutex);
  if (!uimemory_current.is_table()) {
    return;
  }
  toml::table &root_tbl = uimemory_current.as_table();
  const std::string key(item.data(), item.size());
  if (section.empty()) {
    root_tbl.erase(key);
    return;
  }
  auto section_it = root_tbl.find(section);
  if (section_it == root_tbl.end()) {
    return;
  }
  toml::value &sec_val = section_it->second;
  if (!sec_val.is_table()) {
    return;
  }
  toml::table &sec_tbl = sec_val.as_table();
  sec_tbl.erase(key);
}

void Section::remove_section()
{
  memory.ensure_init();
  std::lock_guard<Mutex> lock(uimemory_mutex);
  if (section.empty() || !uimemory_current.is_table()) {
    return;
  }
  toml::table &root_tbl = uimemory_current.as_table();
  root_tbl.erase(section);
}

template std::string Section::get<std::string>(const StringRef item) const;
template void Section::set<std::string>(const StringRef item, const std::string &value);

template char Section::get<char>(const StringRef item) const;
template void Section::set<char>(const StringRef item, const char &value);

template bool Section::get<bool>(const StringRef item) const;
template void Section::set<bool>(const StringRef item, const bool &value);

template int16_t Section::get<int16_t>(const StringRef item) const;
template void Section::set<int16_t>(const StringRef item, const int16_t &value);

template uint16_t Section::get<uint16_t>(const StringRef item) const;
template void Section::set<uint16_t>(const StringRef item, const uint16_t &value);

template int32_t Section::get<int32_t>(const StringRef item) const;
template void Section::set<int32_t>(const StringRef item, const int32_t &value);

template uint32_t Section::get<uint32_t>(const StringRef item) const;
template void Section::set<uint32_t>(const StringRef item, const uint32_t &value);

template int64_t Section::get<int64_t>(const StringRef item) const;
template void Section::set<int64_t>(const StringRef item, const int64_t &value);

template uint64_t Section::get<uint64_t>(const StringRef item) const;
template void Section::set<uint64_t>(const StringRef item, const uint64_t &value);

template float Section::get<float>(const StringRef item) const;
template void Section::set<float>(const StringRef item, const float &value);

template double Section::get<double>(const StringRef item) const;
template void Section::set<double>(const StringRef item, const double &value);

template std::vector<int> Section::get<std::vector<int>>(const StringRef item) const;
template void Section::set<std::vector<int>>(const StringRef item, const std::vector<int> &value);

template std::vector<double> Section::get<std::vector<double>>(const StringRef item) const;
template void Section::set<std::vector<double>>(const StringRef item,
                                                const std::vector<double> &value);

template std::vector<float> Section::get<std::vector<float>>(const StringRef item) const;
template void Section::set<std::vector<float>>(const StringRef item,
                                               const std::vector<float> &value);

/* Global instance */
Memory memory;

}  // namespace blender::ui_memory
