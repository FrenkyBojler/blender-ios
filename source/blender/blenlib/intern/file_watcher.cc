/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 * \brief Linux file watcher using inotify.
 */

#include "BLI_file_watcher.hh"

#include "BLI_fileops.h"
#include "BLI_map.hh"
#include "BLI_memory_cache_file_load.hh"
#include "BLI_path_utils.hh"
#include "BLI_set.hh"

#include <string>

#ifdef __linux__
#  include <sys/inotify.h>
#  include <unistd.h>

namespace blender::file_watcher {

static int fd = -1;
static Map<int, std::string> wd_to_dir;
static Map<std::string, int> dir_to_wd;
static Map<std::string, Set<std::string>> files;

void add_file(StringRef filepath)
{
  if (fd < 0) {
    fd = inotify_init1(IN_NONBLOCK);
    if (fd < 0) {
      return;
    }
  }

  char dir[FILE_MAX];
  BLI_path_split_dir_part(filepath.data(), dir, sizeof(dir));
  std::string dir_str(dir);
  std::string filename(BLI_path_basename(filepath.data()));

  if (!dir_to_wd.contains(dir_str)) {
    int wd = inotify_add_watch(fd, dir, IN_MODIFY | IN_CREATE | IN_MOVED_TO);
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
  if (fd < 0) {
    return false;
  }

  bool any_changed = false;
  char buffer[4096] __attribute__((aligned(__alignof__(struct inotify_event))));

  while (true) {
    ssize_t len = read(fd, buffer, sizeof(buffer));
    if (len <= 0) {
      break;
    }

    const struct inotify_event *event;
    for (char *ptr = buffer; ptr < buffer + len; ptr += sizeof(*event) + event->len) {
      event = (const struct inotify_event *)ptr;

      if (event->len == 0 || !wd_to_dir.contains(event->wd)) {
        continue;
      }

      const std::string &dir_str = wd_to_dir.lookup(event->wd);
      if (!files.contains(dir_str)) {
        continue;
      }

      std::string filename(event->name);
      if (files.lookup(dir_str).contains(filename)) {
        char path[FILE_MAX];
        BLI_path_join(path, sizeof(path), dir_str.c_str(), filename.c_str());
        memory_cache::invalidate_file(path);
        any_changed = true;
      }
    }
  }

  return any_changed;
}

}  // namespace blender::file_watcher

#endif
