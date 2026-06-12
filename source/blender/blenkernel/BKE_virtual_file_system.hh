/* SPDX-FileCopyrightText: 2026 Blender Authors */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file BKE_virtual_file_system.hh
 * \ingroup bke
 * \brief Virtual filesystem core types and backends interface.
 *
 * Provides a unified protocol://path addressing scheme for browsing
 * local and remote (WebDAV) resources through the file browser.
 * Only read-only directory listings are supported in this prototype.
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

  std::unique_ptr<VFSBackend> get_backend();
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
/** \name VFSResult – backend response
 * \{ */

struct VFSResult {
  bool success = true;
  std::string error_message;
  std::vector<VFSEntry> entries;

  static VFSResult from_error(const char *error_msg) noexcept;
};

/* \} */

/* -------------------------------------------------------------------- */
/** \name VFSBackend interface – polymorphic dispatch
 * \{ */

class VFSBackend {
 public:
  virtual ~VFSBackend() = default;

  /** List directory contents. Returns (name, is_dir) pairs. */
  virtual VFSResult list_directory(const VFSPath &path) const = 0;
  
  /** Create a single directory at path within this VFS. Parent must already exist. */
  virtual VFSResult create_directory(const VFSPath &path) const = 0;

  /** Rename an existing entry from src to dst. Both must share the same parent VFSPath (rename, not move). */
  virtual VFSResult rename_item(const VFSPath &src, const VFSPath &dst) const = 0;
};

/** Factory for the local filesystem backend. */
std::unique_ptr<VFSBackend> get_file_system_backend() noexcept;

/** Factory for the Python-based WebDAV backend. */
std::unique_ptr<VFSBackend> get_python_wrapper(const char *class_name) noexcept;

/* \} */

}  // namespace blender::vse
