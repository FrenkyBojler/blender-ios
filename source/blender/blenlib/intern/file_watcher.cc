/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 * \brief Cross-platform file watcher using dmon library.
 */

#include "BLI_file_watcher.hh"

#include "BLI_fileops.h"
#include "BLI_map.hh"
#include "BLI_memory_cache_file_load.hh"
#include "BLI_path_utils.hh"
#include "BLI_set.hh"
#include "BLI_vector.hh"

#include "dmon.h"

#include <memory>
#include <mutex>
#include <string>

namespace blender::file_watcher {

/** Directory watch data passed to dmon callbacks. */
struct WatchData {
  dmon_watch_id watch_id;
  Set<std::string> watched_files;
};

static bool initialized = false;
static Map<std::string, std::unique_ptr<WatchData>> dir_to_watch_data;
static Vector<std::string> changed_files;
static std::mutex changed_files_mutex; /* Protects changed_files from concurrent access. */

/**
 * Callback function for dmon file change events.
 *
 * \param watch_id: The watch ID that triggered this event
 * \param action: The type of change (CREATE, DELETE, MODIFY, MOVE)
 * \param rootdir: The root directory being watched
 * \param filepath: The relative path of the file that changed
 * \param user: User data pointer - points to WatchData for this directory
 */
static void watch_callback(
    dmon_watch_id watch_id,
    dmon_action action,
    const char *rootdir,
    const char *filepath,
    const char * /*oldfilepath*/, /* Not used - we don't handle MOVE actions. */
    void *user)
{
  (void)watch_id;
  (void)action;

  const WatchData *watch_data = static_cast<const WatchData *>(user);
  if (!watch_data) {
    return;
  }

  const StringRef filename(BLI_path_basename(filepath));
  if (!watch_data->watched_files.contains_as(filename)) {
    return;
  }

  char full_path[FILE_MAX];
  BLI_path_join(full_path, sizeof(full_path), rootdir, filepath);
  memory_cache::invalidate_file(full_path);

  /* Thread-safe append: dmon callbacks run on a separate thread. */
  {
    std::lock_guard<std::mutex> lock(changed_files_mutex);
    changed_files.append(full_path);
  }
}

void add_file(StringRef filepath)
{
  if (!initialized) {
    dmon_init();
    initialized = true;
  }

  char dir[FILE_MAX];
  BLI_path_split_dir_part(filepath.data(), dir, sizeof(dir));
  const std::string dir_str(dir);
  const std::string filename(BLI_path_basename(filepath.data()));

  WatchData *watch_data;
  if (!dir_to_watch_data.contains(dir_str)) {
    auto new_watch_data = std::make_unique<WatchData>();
    dmon_watch_id wd = dmon_watch(dir_str.c_str(), watch_callback, 0, new_watch_data.get());

    if (wd.id == 0) {
      return;
    }

    new_watch_data->watch_id = wd;
    watch_data = new_watch_data.get();
    dir_to_watch_data.add(dir_str, std::move(new_watch_data));
  }
  else {
    watch_data = dir_to_watch_data.lookup(dir_str).get();
  }

  watch_data->watched_files.add(filename);
}

void remove_file(StringRef filepath)
{
  char dir[FILE_MAX];
  BLI_path_split_dir_part(filepath.data(), dir, sizeof(dir));
  const std::string dir_str(dir);
  const std::string filename(BLI_path_basename(filepath.data()));

  if (!dir_to_watch_data.contains(dir_str)) {
    return;
  }

  WatchData *watch_data = dir_to_watch_data.lookup(dir_str).get();
  watch_data->watched_files.remove(filename);

  if (watch_data->watched_files.is_empty()) {
    dmon_unwatch(watch_data->watch_id);
    dir_to_watch_data.remove(dir_str);
  }
}

/**
 * Poll for file changes and return the list of files that changed.
 *
 * \return: Vector of full file paths that changed since the last poll
 */
Vector<std::string> poll_changed_files()
{
  if (!initialized) {
    return {};
  }

  /* Thread-safe swap: dmon callbacks run on a separate thread. */
  std::lock_guard<std::mutex> lock(changed_files_mutex);
  Vector<std::string> result = std::move(changed_files);
  changed_files.clear();
  return result;
}

}  // namespace blender::file_watcher
