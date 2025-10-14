/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 * \brief File watcher using efsw library.
 */

#include "BLI_file_watcher.hh"

#include "BLI_fileops.h"
#include "BLI_map.hh"
#include "BLI_memory_cache_file_load.hh"
#include "BLI_path_utils.hh"
#include "BLI_set.hh"
#include "efsw/efsw.hpp"

#include <string>

namespace blender::file_watcher {

static efsw::FileWatcher *file_watcher = nullptr;
static Map<efsw::WatchID, std::string> wd_to_dir;
static Map<std::string, efsw::WatchID> dir_to_wd;
static Map<std::string, Set<std::string>> files;
static bool any_changed = false;

class FileWatcherListener : public efsw::FileWatchListener {
 public:
  void handleFileAction(efsw::WatchID watchid,
                        const std::string &dir,
                        const std::string &filename,
                        efsw::Action action,
                        std::string oldFilename) override
  {
    (void)watchid;
    (void)oldFilename;

    if (!wd_to_dir.contains(watchid)) {
      return;
    }

    const std::string &dir_str = wd_to_dir.lookup(watchid);
    if (!files.contains(dir_str)) {
      return;
    }

    if (files.lookup(dir_str).contains(filename)) {
      char path[FILE_MAX];
      BLI_path_join(path, sizeof(path), dir_str.c_str(), filename.c_str());
      memory_cache::invalidate_file(path);
      any_changed = true;
    }
  }
};

static FileWatcherListener *listener = nullptr;

void add_file(StringRef filepath)
{
  if (!file_watcher) {
    file_watcher = new efsw::FileWatcher();
    listener = new FileWatcherListener();
    file_watcher->watch();
  }

  char dir[FILE_MAX];
  BLI_path_split_dir_part(filepath.data(), dir, sizeof(dir));
  std::string dir_str(dir);
  std::string filename(BLI_path_basename(filepath.data()));

  if (!dir_to_wd.contains(dir_str)) {
    efsw::WatchID wd = file_watcher->addWatch(dir_str, listener, false);
    if (wd >= 0) {
      wd_to_dir.add(wd, dir_str);
      dir_to_wd.add(dir_str, wd);
    }
  }

  if (!files.contains(dir_str)) {
    files.add(dir_str, Set<std::string>());
  }
  files.lookup(dir_str).add(filename);
}

void remove_file(StringRef filepath)
{
  char dir[FILE_MAX];
  BLI_path_split_dir_part(filepath.data(), dir, sizeof(dir));
  std::string dir_str(dir);
  std::string filename(BLI_path_basename(filepath.data()));

  if (files.contains(dir_str)) {
    files.lookup(dir_str).remove(filename);
  }
}

bool poll()
{
  if (!file_watcher) {
    return false;
  }

  bool result = any_changed;
  any_changed = false;
  return result;
}

}  // namespace blender::file_watcher
