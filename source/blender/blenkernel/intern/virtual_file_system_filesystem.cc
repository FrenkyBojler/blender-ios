/* SPDX-FileCopyrightText: 2026 Blender Authors */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file virtual_file_system_filesystem.cc
 * \ingroup bke
 * Local filesystem backend for VFS (POSIX / Windows).
 */

#include "BKE_virtual_file_system.hh"
#include "BLI_assert.hh"
#include "BLI_fileops.hh"
#include "CLG_log.h"

#ifdef WIN32
#  include <direct.h>
#  include <io.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#endif

static CLG_LogRef LOG = {"vfs.local"};

namespace blender::vse {

/* -------------------------------------------------------------------- */
/** \name FileSystemBackend - real POSIX/Windows local filesystem backend. */

class FileSystemBackend final : public VFSBackend {
 public:
   ~FileSystemBackend() = default;
   VFSResult<std::vector<VFSEntry>> list_directory(const VFSPath &path) const override;
   VFSResult<bool> create_directory(const VFSPath &path) const override;
   VFSResult<bool> rename_item(const VFSPath &src, const VFSPath &dst) const override;
   VFSResult<bool> exists(const VFSPath &path) const override;
   VFSResult<bool> delete_item(const VFSPath &path) const override;
 };

#ifdef WIN32

VFSResult<std::vector<VFSEntry>> FileSystemBackend::list_directory(const VFSPath &path) const
{
  if (path.path.empty()) {
    CLOG_WARN(&LOG, "FileSystemBackend::list_directory: empty path");
    return VFSResult<std::vector<VFSEntry>>::from_error("Empty path");
  }

  std::string win_path = path.path.substr(1);
  for (auto &c : win_path) {
    if (c == '/')
      c = '\\';
  }

  char pattern[MAX_PATH];
  snprintf(pattern, sizeof(pattern), "%s\\*", win_path.c_str());

  _finddata64_t fi;
  longptr_t ptr = _findfirst64(pattern, &fi);
  if (ptr == -1) {
    CLOG_WARN(&LOG, "FileSystemBackend::list_directory: opendir('%s') failed", path.path.c_str());
    return VFSResult<std::vector<VFSEntry>>::from_error("Could not open directory");
  }

  VFSResult<std::vector<VFSEntry>> result;
  result.value.emplace();
  do {
    if (fi.name[0] == '.' && (fi.name[1] == '\0' || (fi.name[1] == '.' && fi.name[2] == '\0')))
      continue;

    VFSEntry e;
    e.name = fi.name;
    bool is_dir = (fi.attrib & _A_SUBDIR) != 0;
    e.is_directory = is_dir;
    if (is_dir) {
      e.flags |= VFSEntryFlags::IsDirectory;
    }
    if (fi.attrib & _A_HIDDEN) {
      e.flags |= VFSEntryFlags::IsHidden;
    }
    e.size = fi.size;
    e.last_modification_time = fi.time_write;
    result.value->push_back(std::move(e));
  } while (_findnext64(ptr, &fi) == 0);
  _findclose(ptr);
  CLOG_DEBUG(&LOG,
             "FileSystemBackend::list_directory('%s'): %zu entries",
             path.path.c_str(),
             result.value->size());
  return result;
}

#else /* POSIX */

VFSResult<std::vector<VFSEntry>> FileSystemBackend::list_directory(const VFSPath &path) const
{
  BLI_assert(!path.path.empty());
  if (path.path.empty()) {
    CLOG_WARN(&LOG, "FileSystemBackend::list_directory: empty path");
    return VFSResult<std::vector<VFSEntry>>::from_error("Empty path");
  }

  DIR *dir = opendir(path.path.c_str());
  if (!dir) {
    std::string err = std::string("opendir('") + path.path + "') failed";
    CLOG_WARN(&LOG, "FileSystemBackend::list_directory: %s", err.c_str());
    return VFSResult<std::vector<VFSEntry>>::from_error(err.c_str());
  }

  VFSResult<std::vector<VFSEntry>> result;
  result.value.emplace();
  struct dirent *de;
  while ((de = readdir(dir)) != nullptr) {
    if (de->d_name[0] == '.' &&
        (de->d_name[1] == '\0' || (de->d_name[1] == '.' && de->d_name[2] == '\0')))
      continue;

    std::string full = path.path + "/" + de->d_name;
    struct stat st;
    if (stat(full.c_str(), &st) != 0)
      continue;

    VFSEntry e;
    e.name = de->d_name;
    bool is_dir = S_ISDIR(st.st_mode);
    e.is_directory = is_dir;
    if (is_dir) {
      e.flags |= VFSEntryFlags::IsDirectory;
    }
    if (de->d_name[0] == '.') {
      e.flags |= VFSEntryFlags::IsHidden;
    }
    e.size = st.st_size;
    e.last_modification_time = st.st_mtime;
    result.value->push_back(std::move(e));
  }
  closedir(dir);
  CLOG_DEBUG(&LOG,
             "FileSystemBackend::list_directory('%s'): %zu entries",
             path.path.c_str(),
             result.value->size());
  return result;
}

#endif /* WIN32 */

/* -------------------------------------------------------------------- */
/** \name FileSystemBackend - create_directory, rename_item, and exists */
VFSResult<bool> FileSystemBackend::create_directory(const VFSPath &path) const
{
  if (path.path.empty()) {
    CLOG_WARN(&LOG, "FileSystemBackend::create_directory: empty path");
    return VFSResult<bool>::from_error("Empty path");
  }

#ifdef WIN32
  std::string win_path = path.path.substr(1);
  for (auto &c : win_path) {
    if (c == '/')
      c = '\\';
  }
  if (_mkdir(win_path.c_str()) != 0) {
    std::string err = std::string("mkdir('") + win_path + "') failed";
    CLOG_WARN(&LOG, "FileSystemBackend::create_directory: %s", err.c_str());
    return VFSResult<bool>::from_error(err.c_str());
  }
#else
  if (mkdir(path.path.c_str(), 0755) != 0) {
    std::string err = std::string("mkdir('") + path.path + "') failed";
    CLOG_WARN(&LOG, "FileSystemBackend::create_directory: %s", err.c_str());
    return VFSResult<bool>::from_error(err.c_str());
  }
#endif
  CLOG_DEBUG(&LOG, "FileSystemBackend::create_directory('%s'): success", path.path.c_str());
  return VFSResult<bool>{true};
}

VFSResult<bool> FileSystemBackend::exists(const VFSPath &path) const
{
  if (path.path.empty()) {
    return VFSResult<bool>::from_error("Empty path");
  }
  return VFSResult<bool>{BLI_exists(path.path.c_str())};
}

VFSResult<bool> FileSystemBackend::delete_item(const VFSPath &path) const
{
  if (path.path.empty()) {
    return VFSResult<bool>::from_error("Empty path");
  }

  const char *r_error_message = nullptr;
  if (BLI_delete_soft(path.path.c_str(), &r_error_message) != 0 || BLI_exists(path.path.c_str())) {
    CLOG_WARN(&LOG, "FileSystemBackend::delete_item('%s'): failed", path.path.c_str());
    return VFSResult<bool>::from_error(r_error_message ? r_error_message : "delete failed");
  }
  CLOG_DEBUG(&LOG, "FileSystemBackend::delete_item('%s'): success", path.path.c_str());
  return VFSResult<bool>{true};
}

VFSResult<bool> FileSystemBackend::rename_item(const VFSPath &src, const VFSPath &dst) const
{
  if (src.path.empty() || dst.path.empty()) {
    CLOG_WARN(&LOG, "FileSystemBackend::rename_item: empty path");
    return VFSResult<bool>::from_error("Empty path");
  }

#ifdef WIN32
  std::string src_win = src.path.substr(1);
  std::string dst_win = dst.path.substr(1);
  for (auto &c : src_win) {
    if (c == '/')
      c = '\\';
  }
  for (auto &c : dst_win) {
    if (c == '/')
      c = '\\';
  }

  if (_wrename(reinterpret_cast<const wchar_t *>(src_win.c_str()),
               reinterpret_cast<const wchar_t *>(dst_win.c_str())) != 0)
  {
    std::string err = std::string("rename('") + src_win + "' -> '" + dst_win + "') failed";
    CLOG_WARN(&LOG, "FileSystemBackend::rename_item: %s", err.c_str());
    return VFSResult<bool>::from_error(err.c_str());
  }
#else
  if (rename(src.path.c_str(), dst.path.c_str()) != 0) {
    std::string err = std::string("rename('") + src.path + "' -> '" + dst.path + "') failed";
    CLOG_WARN(&LOG, "FileSystemBackend::rename_item: %s", err.c_str());
    return VFSResult<bool>::from_error(err.c_str());
  }
#endif
  CLOG_DEBUG(&LOG,
             "FileSystemBackend::rename_item('%s' -> '%s'): success",
             src.path.c_str(),
             dst.path.c_str());
  return VFSResult<bool>{true};
}

std::unique_ptr<VFSBackend> get_file_system_backend() noexcept
{
  return std::make_unique<FileSystemBackend>();
}

}  // namespace blender::vse
