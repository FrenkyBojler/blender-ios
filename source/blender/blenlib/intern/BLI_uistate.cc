/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 */

#include "../../../extern/toml11/toml.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "BKE_appdir.hh"

#include "BLI_fileops.h"
#include "BLI_mutex.hh"
#include "BLI_path_utils.hh"
#include "BLI_uistate.hh"

namespace blender {

constexpr toml::spec version = toml::spec::v(1, 1, 0);
#define BLI_UISTATE_FILE_NAME "uistate.toml"

toml::value uistate_current;
toml::value uistate_default;

static Mutex uistate_mutex;
static Mutex uistate_init_mutex;
static std::atomic<bool> uistate_ready{false};
static std::condition_variable_any uistate_init_cv;
static std::once_flag uistate_init_once;

static std::string uistate_file_path()
{
  std::optional<std::string> datafiles_path = BKE_appdir_folder_id(BLENDER_USER_CONFIG, "");
  if (datafiles_path.has_value()) {
    return *datafiles_path + SEP + BLI_UISTATE_FILE_NAME;
  }
  return {};
}

static void bli_uistate_print_errors(std::vector<toml::error_info> errors)
{
  for (auto error : errors) {
    std::string msg = toml::format_error(error);
    printf("%s\n", msg.c_str());
  }
}

static void bli_uistate_init()
{
  /* Load defaults. */
  toml::result result = toml::try_parse_str(default_uistate_toml, version);
  if (result.is_ok()) {
    std::lock_guard<Mutex> lock(uistate_mutex);
    uistate_default = result.unwrap();
  }
  else {
    bli_uistate_print_errors(result.unwrap_err());
  }

  /* Load from on-disk file if found. */
  if (BLI_exists(uistate_file_path().c_str())) {
    /* Read existing uistate file. */
    toml::result result = toml::try_parse(uistate_file_path(), version);
    if (result.is_ok()) {
      std::lock_guard<Mutex> lock(uistate_mutex);
      uistate_current = result.unwrap();
    }
    else {
      bli_uistate_print_errors(result.unwrap_err());
    }
  }
  else {
    /* Create a new uistate file from defaults. */
    {
      std::lock_guard<Mutex> lock(uistate_mutex);
      uistate_current = uistate_default;
    }
    BLI_uistate_save();
  }

  /* Mark ready and wake any waiters (covers both sync and async init). */
  uistate_ready.store(true, std::memory_order_release);
  uistate_init_cv.notify_all();
}

void BLI_uistate_init_async()
{
  /* Ensure we only start one background init thread. `bli_uistate_init()`
   * itself sets `uistate_ready` and notifies waiters. */
  std::call_once(uistate_init_once, []() { std::thread([]() { bli_uistate_init(); }).detach(); });
}

static void bli_uistate_ensure_init()
{
  if (uistate_ready.load(std::memory_order_acquire)) {
    return;
  }

  printf("WARNING: Waiting for BLI_uistate_init_async() to complete.\n");
  std::unique_lock<Mutex> lock(uistate_init_mutex);
  uistate_init_cv.wait(lock, [] { return uistate_ready.load(std::memory_order_acquire); });
}

bool BLI_uistate_save()
{
  std::lock_guard<Mutex> lock(uistate_mutex);
  if (uistate_current.is_empty()) {
    return false;
  }
  std::string s = toml::format(uistate_current, version);
  FILE *fp = BLI_fopen(uistate_file_path().c_str(), "w");
  if (fp == nullptr) {
    return false;
  }
  fputs(s.c_str(), fp);
  fclose(fp);
  return true;
}

void UIState::remove(const StringRef item)
{
  bli_uistate_ensure_init();
  std::lock_guard<Mutex> lock(uistate_mutex);

  if (!uistate_current.is_table()) {
    return;
  }

  toml::table &root_tbl = uistate_current.as_table();
  if (section.is_empty()) {
    root_tbl.erase(item);
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
  sec_tbl.erase(item);
}

void UIState::remove_section()
{
  bli_uistate_ensure_init();
  std::lock_guard<Mutex> lock(uistate_mutex);
  if (section.is_empty() || !uistate_current.is_table()) {
    return;
  }
  toml::table &root_tbl = uistate_current.as_table();
  root_tbl.erase(section);
}

template<typename T> T UIState::get(const StringRef item) const
{
  bli_uistate_ensure_init();
  std::lock_guard<Mutex> lock(uistate_mutex);
  const std::string sec = section;
  const std::string key = item;
  const toml::value &cur = sec.empty() ? uistate_current[key] : uistate_current[sec][key];
  const toml::value &def = sec.empty() ? uistate_default[key] : uistate_default[sec][key];
  return toml::get_or(cur, toml::get_or(def, T{}));
}

template<typename T> void UIState::set(const StringRef item, const T &value)
{
  bli_uistate_ensure_init();
  std::lock_guard<Mutex> lock(uistate_mutex);
  if (section.is_empty()) {
    uistate_current[item] = value;
  }
  else {
    uistate_current[section][item] = value;
  }
}

template std::string UIState::get<std::string>(const StringRef item) const;
template void UIState::set<std::string>(const StringRef item, const std::string &value);

template char UIState::get<char>(const StringRef item) const;
template void UIState::set<char>(const StringRef item, const char &value);
template bool UIState::get<bool>(const StringRef item) const;
template void UIState::set<bool>(const StringRef item, const bool &value);

template int16_t UIState::get<int16_t>(const StringRef item) const;
template void UIState::set<int16_t>(const StringRef item, const int16_t &value);

template uint16_t UIState::get<uint16_t>(const StringRef item) const;
template void UIState::set<uint16_t>(const StringRef item, const uint16_t &value);

template int32_t UIState::get<int32_t>(const StringRef item) const;
template void UIState::set<int32_t>(const StringRef item, const int32_t &value);

template uint32_t UIState::get<uint32_t>(const StringRef item) const;
template void UIState::set<uint32_t>(const StringRef item, const uint32_t &value);

template int64_t UIState::get<int64_t>(const StringRef item) const;
template void UIState::set<int64_t>(const StringRef item, const int64_t &value);

template uint64_t UIState::get<uint64_t>(const StringRef item) const;
template void UIState::set<uint64_t>(const StringRef item, const uint64_t &value);

template float UIState::get<float>(const StringRef item) const;
template void UIState::set<float>(const StringRef item, const float &value);

template double UIState::get<double>(const StringRef item) const;
template void UIState::set<double>(const StringRef item, const double &value);

template std::vector<int> UIState::get<std::vector<int>>(const StringRef item) const;
template void UIState::set<std::vector<int>>(const StringRef item, const std::vector<int> &value);

template std::vector<double> UIState::get<std::vector<double>>(const StringRef item) const;
template void UIState::set<std::vector<double>>(const StringRef item,
                                                const std::vector<double> &value);

template std::vector<float> UIState::get<std::vector<float>>(const StringRef item) const;
template void UIState::set<std::vector<float>>(const StringRef item,
                                               const std::vector<float> &value);

}  // namespace blender
