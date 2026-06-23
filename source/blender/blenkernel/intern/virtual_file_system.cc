/* SPDX-FileCopyrightText: 2026 Blender Authors */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file virtual_file_system.cc
 */

#include "BKE_virtual_file_system.hh"
#include "BLI_assert.hh"
#include "CLG_log.h"

static CLG_LogRef LOG = {"vfs"};

#include <cctype>
#include <cstring>

namespace {

const char *skip_prefix(const char *text, const char *prefix) noexcept
{
  size_t len = strlen(prefix);
  if (strncmp(text, prefix, len) == 0) {
    return text + len;
  }
  return nullptr;
}

void vfs_trim_path(std::string &path) noexcept
{
  if (path.empty()) {
    path = "/";
    return;
  }
  /* Normalize Windows backslashes to forward slashes. */
  for (auto &c : path) {
    if (c == '\\') {
      c = '/';
    }
  }
  /* Windows drive letter (e.g., "C:\..." or "C:/...") — no leading slash needed. */
  if (path.size() >= 2 && std::isalpha(path[0]) && path[1] == ':') {
    return;
  }
  if (path.front() != '/') {
    path.insert(0, "/", 1);
  }
}

} /* namespace */

namespace blender::vfs {

std::optional<VFSPath> VFSPath::parse(const char *input_nullable)
{
  if (!input_nullable || !*input_nullable) {
    CLOG_TRACE(&LOG, "VFSPath::parse(null|empty) -> file:/");
    VFSPath r{};
    r.protocol = VFSProtocol::FileSystem;
    r.path = "/";
    return r;
  }

  const char *after = skip_prefix(input_nullable, "webdav://");
  if (after) {
    VFSPath r{};
    r.protocol = VFSProtocol::WebDAV;
    const char *slash = strchr(after, '/');
    if (slash) {
      r.endpoint.assign(after, slash);
      r.path = slash;
    }
    else {
      r.endpoint = after;
      r.path = "/";
    }
    CLOG_TRACE(&LOG,
               "VFSPath::parse('%s') -> webdav://%s%s",
               input_nullable,
               r.endpoint.c_str(),
               r.path.c_str());
    return r;
  }

  after = skip_prefix(input_nullable, "file:///");
  if (after) {
    VFSPath r{};
    r.protocol = VFSProtocol::FileSystem;
    r.path.assign(after);
    vfs_trim_path(r.path);
    CLOG_TRACE(&LOG, "VFSPath::parse('%s') -> %s", input_nullable, r.to_string().c_str());
    return r;
  }

  VFSPath r{};
  r.protocol = VFSProtocol::FileSystem;
  const char *raw = (input_nullable[0] == '~') ? input_nullable + 1 : input_nullable;
  if (*raw == '/') {
    raw++;
  }
  r.path.assign(raw);
  vfs_trim_path(r.path);
  CLOG_TRACE(&LOG, "VFSPath::parse('%s') -> %s", input_nullable, r.to_string().c_str());
  return r;
}

std::string VFSPath::to_string() const
{
  switch (protocol) {
    case VFSProtocol::Invalid:
      return "(error)";
    case VFSProtocol::FileSystem:
      /* Ensure triple-slash "file:///" form: for Unix paths that start with '/',
       * "file://" + path already gives the three slashes. For Windows drive-letter
       * paths (e.g., "C:/Users"), insert the authority separator slash. */
      if (!path.empty() && path[0] == '/') {
        return "file://" + path;
      }
      return "file:///" + path;
    case VFSProtocol::WebDAV:
      return "webdav://" + endpoint + path;
  }
  BLI_assert_unreachable();
  return "(error)";
}

void VFSPath::normalize()
{
  if (path.empty()) {
    path = "/";
    return;
  }

  /* Windows drive letter path (e.g., "C:/Users"): just ensure trailing slash.
   * The component-splitting logic below assumes a leading '/'. */
  if (path.size() >= 2 && std::isalpha(path[0]) && path[1] == ':') {
    if (path.back() != '/') {
      path.push_back('/');
    }
    return;
  }

  /* Ensure leading slash. */
  if (path.front() != '/') {
    path.insert(0, "/");
  }

  /* Collapse "/./" and resolve "/../" components in-place. */
  std::vector<std::string_view> components;
  const char *start = path.c_str() + 1;
  while (*start) {
    const char *slash = strchr(start, '/');
    if (slash) {
      components.emplace_back(start, slash - start);
      start = slash + 1;
    }
    else {
      components.emplace_back(start);
      break;
    }
  }

  std::vector<std::string_view> resolved;
  for (std::string_view comp : components) {
    if (comp == "." || comp.empty()) {
      continue;
    }
    if (comp == "..") {
      if (!resolved.empty()) {
        resolved.pop_back();
      }
      /* If already at root, stay at root. */
      continue;
    }
    resolved.push_back(comp);
  }

  std::string result;
  result.reserve(path.size());
  result.push_back('/');
  for (size_t i = 0; i < resolved.size(); i++) {
    if (i > 0) {
      result.push_back('/');
    }
    result.append(resolved[i]);
  }

  path = std::move(result);
}

VFSPath VFSPath::parent() const
{
  VFSPath p;
  p.protocol = protocol;
  p.endpoint = endpoint;

  if (is_root()) {
    p.path = "/";
    return p;
  }

  std::string pstr = path;
  /* Strip trailing slash. */
  if (pstr.size() > 1 && pstr.back() == '/') {
    pstr.pop_back();
  }
  /* Strip last component. */
  const size_t slash = pstr.rfind('/');
  if (slash != std::string::npos) {
    pstr = pstr.substr(0, slash);
  }
  if (pstr.empty()) {
    pstr = "/";
  }
  p.path = std::move(pstr);
  return p;
}

VFSPath VFSPath::join(std::string_view component) const
{
  VFSPath result;
  result.protocol = protocol;
  result.endpoint = endpoint;

  if (component.empty()) {
    result.path = path;
    return result;
  }

  std::string joined = path;
  if (joined.back() != '/') {
    joined.push_back('/');
  }
  /* Skip leading slashes on component. */
  const char *comp = component.data();
  while (*comp == '/') {
    comp++;
  }
  joined.append(comp);

  result.path = std::move(joined);
  return result;
}

std::unique_ptr<VFSBackend> VFSPath::get_backend() const
{
  std::unique_ptr<VFSBackend> result;
  switch (protocol) {
    case VFSProtocol::Invalid:
      result = nullptr;
      break;
    case VFSProtocol::FileSystem:
      result = get_file_system_backend();
      break;
    case VFSProtocol::WebDAV:
      result = get_python_wrapper("_bpy_internal.vfs.webdav.VFSWebDAV");
      break;
  }
  return result;
}

}  // namespace blender::vfs
