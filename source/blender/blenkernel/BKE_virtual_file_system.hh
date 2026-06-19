/* SPDX-FileCopyrightText: 2026 Blender Authors */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file BKE_virtual_file_system.hh
 * \ingroup bke
 * \brief Virtual filesystem core types and backends interface.
 *
 * Provides a unified protocol://path addressing scheme for browsing
 * local and remote (WebDAV) resources through the file browser.
 * Supports listing, directory creation, renaming, and existence checks.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "BLI_enum_flags.hh"

namespace blender::vse {

class VFSBackend;

// Setup docker webdav:
// `docker run --name webdav-singularity -p 8080:80 -v
// /home/jeroen/productions/singularity/:/media/data -d sfuhrm/docker-nginx-webdav`

/* -------------------------------------------------------------------- */
/** \name Protocol identification
 * \{ */

enum class VFSProtocol : signed char {
  Invalid = -1,
  FileSystem = 0,
  WebDAV = 1,
  // TODO(jbakker): Add Memory for loading resources that are compiled in (factory-startup, default
  // scene, splash screen).
};
/* \} */

/* -------------------------------------------------------------------- */
/** \name VFS path representation
 * \{ */

// TODO(jbakker): We should also add a

struct VFSPath {
  VFSProtocol protocol = VFSProtocol::FileSystem;
  std::string endpoint; /* "" for local, "127.0.0.1:8080" for WebDAV */
  std::string path;     /* always starts with '/' or is empty (defaults to '/') */

  /** Parse a protocol://path string into a VFSPath struct.
   * Recognized schemes: webdav://, file:///
   * No recognized scheme defaults to Local protocol. */
  static std::optional<VFSPath> parse(const char *input_nullable);

  /** Serialize back to canonical form. */
  std::string to_string() const;

  /** Resolve `..` and `.` components, ensure leading `/`. */
  void normalize();

  /** Return the parent directory VFSPath. */
  VFSPath parent() const;

  /** Append a path component. */
  VFSPath join(std::string_view component) const;

  /** True if path is `/` or empty. */
  bool is_root() const
  {
    return path == "/" || path.empty();
  }

  /** True if protocol is not the local filesystem. */
  bool is_virtual() const
  {
    return protocol != VFSProtocol::FileSystem;
  }

  std::unique_ptr<VFSBackend> get_backend() const;
};
/* \} */

/* -------------------------------------------------------------------- */
/** \name VFSEntryFlags – entry type and metadata flags
 * \{ */

enum class VFSEntryFlags : uint8_t {
  None = 0,
  IsDirectory = (1 << 0),
  IsHidden = (1 << 1),
};

ENUM_OPERATORS(VFSEntryFlags)

/* \} */

/* -------------------------------------------------------------------- */
/** \name VFSEntry – result entry with name, type, and stat fields
 * \{ */

struct VFSEntry {
  std::string name;
  VFSEntryFlags flags = VFSEntryFlags::None;
  bool is_directory = false;
  int64_t last_modification_time = -1; /**< Unix epoch seconds (−1 = unknown). */
  /** File size in bytes. */
  uint64_t size = 0;
};

/* \} */

/* -------------------------------------------------------------------- */
/** \name VFSResult – backend response with typed value and error handling
 * \{ */

template<typename T = void> struct VFSResult {
  bool success = true;
  std::string error_message;
  std::optional<T> value;

  static VFSResult from_error(const char *error_msg) noexcept
  {
    VFSResult r{};
    r.success = false;
    if (error_msg)
      r.error_message = error_msg;
    return r;
  }
};

/* \} */

/* -------------------------------------------------------------------- */
/** \name VFSBackend interface – polymorphic dispatch
 * \{ */

class VFSBackend {
 public:
  virtual ~VFSBackend() = default;

  /** List directory contents. Returns entries for the requested path. */
  virtual VFSResult<std::vector<VFSEntry>> list_directory(const VFSPath &path) const = 0;

  /** Create a single directory at path within this VFS. Parent must already exist. Returns true if
   * successful. */
  virtual VFSResult<bool> create_directory(const VFSPath &path) const = 0;

  /** Rename an existing entry from src to dst. Both must share the same parent VFSPath (rename,
   * not move). Returns true if successful. */
  virtual VFSResult<bool> rename_item(const VFSPath &src, const VFSPath &dst) const = 0;

  /** Check if a path exists in this VFS. */
  virtual VFSResult<bool> exists(const VFSPath &path) const = 0;

  /** Delete an item (file or directory) at path. Returns true if successful. */
  virtual VFSResult<bool> delete_item(const VFSPath &path) const = 0;
};

/** Factory for the local filesystem backend. */
std::unique_ptr<VFSBackend> get_file_system_backend() noexcept;

/** Factory for the Python-based WebDAV backend. */
std::unique_ptr<VFSBackend> get_python_wrapper(const char *class_name) noexcept;

/* \} */

}  // namespace blender::vse
