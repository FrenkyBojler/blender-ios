/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * Implementation of MemoryFile / MemorySection backed by TOML.
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

namespace blender {

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
  /* Parse defaults from header literal. */
  toml::result default_result = toml::try_parse_str(default_toml, version);
  if (default_result.is_ok()) {
    std::lock_guard<Mutex> lock(uimemory_mutex);
    uimemory_default = default_result.unwrap();
  }
  else {
    uimemory_print_errors(default_result.unwrap_err());
  }

  /* Load from on-disk file if found. */
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
  else {
    /* No file: initialize from defaults and save. */
    {
      std::lock_guard<Mutex> lock(uimemory_mutex);
      uimemory_current = uimemory_default;
    }
    uimemory.save();
  }

  /* Mark ready and wake any waiters. */
  uimemory_ready.store(true, std::memory_order_release);
  uimemory_init_cv.notify_all();
}

/* Manager implementation (thin wrapper around free functions but exposes ensure_init). */
void MemoryFile::init_async()
{
  std::call_once(uimemory_init_once,
                 []() { std::thread([]() { uimemory_init_impl(); }).detach(); });
}

void MemoryFile::ensure_init()
{
  if (uimemory_ready.load(std::memory_order_acquire)) {
    return;
  }
  /* Wait using BLI Mutex + condition_variable_any. */
  std::unique_lock<Mutex> lock(uimemory_init_mutex);
  uimemory_init_cv.wait(lock, [] { return uimemory_ready.load(std::memory_order_acquire); });
}

bool MemoryFile::save() const
{
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

template<typename T> T MemorySection::get(const StringRef item) const
{
  if (!uimemory_ready.load(std::memory_order_acquire)) {
    std::unique_lock<Mutex> lock(uimemory_init_mutex);
    uimemory_init_cv.wait(lock, [] { return uimemory_ready.load(std::memory_order_acquire); });
  }

  std::lock_guard<Mutex> lock(uimemory_mutex);
  const std::string &sec = section;
  const std::string key(item.data(), item.size());

  const toml::value &cur = sec.empty() ? uimemory_current[key] : uimemory_current[sec][key];
  const toml::value &def = sec.empty() ? uimemory_default[key] : uimemory_default[sec][key];
  return toml::get_or(cur, toml::get_or(def, T{}));
}

template<typename T> void MemorySection::set(const StringRef item, const T &value)
{
  if (!uimemory_ready.load(std::memory_order_acquire)) {
    std::unique_lock<Mutex> lock(uimemory_init_mutex);
    uimemory_init_cv.wait(lock, [] { return uimemory_ready.load(std::memory_order_acquire); });
  }

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

void MemorySection::remove(const StringRef item)
{
  if (!uimemory_ready.load(std::memory_order_acquire)) {
    std::unique_lock<Mutex> lock(uimemory_init_mutex);
    uimemory_init_cv.wait(lock, [] { return uimemory_ready.load(std::memory_order_acquire); });
  }
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

void MemorySection::remove_section()
{
  if (!uimemory_ready.load(std::memory_order_acquire)) {
    std::unique_lock<Mutex> lock(uimemory_init_mutex);
    uimemory_init_cv.wait(lock, [] { return uimemory_ready.load(std::memory_order_acquire); });
  }
  std::lock_guard<Mutex> lock(uimemory_mutex);
  if (section.empty() || !uimemory_current.is_table()) {
    return;
  }
  toml::table &root_tbl = uimemory_current.as_table();
  root_tbl.erase(section);
}

template std::string MemorySection::get<std::string>(const StringRef item) const;
template void MemorySection::set<std::string>(const StringRef item, const std::string &value);

template char MemorySection::get<char>(const StringRef item) const;
template void MemorySection::set<char>(const StringRef item, const char &value);

template bool MemorySection::get<bool>(const StringRef item) const;
template void MemorySection::set<bool>(const StringRef item, const bool &value);

template int16_t MemorySection::get<int16_t>(const StringRef item) const;
template void MemorySection::set<int16_t>(const StringRef item, const int16_t &value);

template uint16_t MemorySection::get<uint16_t>(const StringRef item) const;
template void MemorySection::set<uint16_t>(const StringRef item, const uint16_t &value);

template int32_t MemorySection::get<int32_t>(const StringRef item) const;
template void MemorySection::set<int32_t>(const StringRef item, const int32_t &value);

template uint32_t MemorySection::get<uint32_t>(const StringRef item) const;
template void MemorySection::set<uint32_t>(const StringRef item, const uint32_t &value);

template int64_t MemorySection::get<int64_t>(const StringRef item) const;
template void MemorySection::set<int64_t>(const StringRef item, const int64_t &value);

template uint64_t MemorySection::get<uint64_t>(const StringRef item) const;
template void MemorySection::set<uint64_t>(const StringRef item, const uint64_t &value);

template float MemorySection::get<float>(const StringRef item) const;
template void MemorySection::set<float>(const StringRef item, const float &value);

template double MemorySection::get<double>(const StringRef item) const;
template void MemorySection::set<double>(const StringRef item, const double &value);

template std::vector<int> MemorySection::get<std::vector<int>>(const StringRef item) const;
template void MemorySection::set<std::vector<int>>(const StringRef item,
                                                   const std::vector<int> &value);

template std::vector<double> MemorySection::get<std::vector<double>>(const StringRef item) const;
template void MemorySection::set<std::vector<double>>(const StringRef item,
                                                      const std::vector<double> &value);

template std::vector<float> MemorySection::get<std::vector<float>>(const StringRef item) const;
template void MemorySection::set<std::vector<float>>(const StringRef item,
                                                     const std::vector<float> &value);

/* Global instance */
MemoryFile uimemory;

}  // namespace blender
