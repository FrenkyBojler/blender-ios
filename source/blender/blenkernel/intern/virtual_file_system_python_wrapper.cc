/* SPDX-FileCopyrightText: 2026 Blender Authors */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file virtual_file_system_python_wrapper.cc
 * Bridges VFSBackend calls to Python modules via BPY runtime.
 */

#include "BKE_virtual_file_system.hh"

#include "BKE_idprop.hh"
#include "BLI_memory_utils.hh"
#include "CLG_log.h"

#ifdef WITH_PYTHON
#  include "BPY_extern_run.hh"
#endif

static CLG_LogRef LOG = {"vfs.python"};

namespace blender::vfs {

/* -------------------------------------------------------------------- */
/** \name PythonBackend - generic Python-based VFS backend proxy. */

class PythonBackend final : public VFSBackend {
 public:
  explicit PythonBackend(std::string class_name) : class_name_(std::move(class_name)) {}

  VFSResult<std::vector<VFSEntry>> list_directory(const VFSPath &path) const override;
  VFSResult<bool> create_directory(const VFSPath &path) const override;
  VFSResult<bool> rename_item(const VFSPath &src, const VFSPath &dst) const override;
  VFSResult<bool> exists(const VFSPath &path) const override;
  VFSResult<bool> delete_item(const VFSPath &path) const override;

 private:
#ifdef WITH_PYTHON
  std::string class_name_;
#endif
};

/* Split a.b.c.ClassName into module="a.b.c" and cls_name="ClassName". */
static void parse_qualified_class_name(const std::string &fqcn,
                                       std::string &out_module,
                                       std::string &out_cls)
{
  const size_t dot = fqcn.rfind('.');
  if (dot != std::string::npos && dot > 0) {
    out_module = fqcn.substr(0, dot);
    out_cls = fqcn.substr(dot + 1);
  }
  else {
    out_module.clear();
    out_cls = fqcn;
  }
}

VFSResult<std::vector<VFSEntry>> PythonBackend::list_directory(const VFSPath &path) const
{
  VFSResult<std::vector<VFSEntry>> result;
  if (path.path.empty()) {
    result.success = false;
    result.error_message = "Empty virtual path";
    return result;
  }

#ifdef WITH_PYTHON
  std::string mod;
  std::string cls;
  parse_qualified_class_name(class_name_, mod, cls);

  /* Build the import string dynamically. */
  std::string script_str;
  if (mod.empty()) {
    script_str = "from " + cls + " import VFSWebDAV as _vfs_cls\n";
  }
  else {
    script_str = "from " + mod + " import " + cls + " as _vfs_cls\n";
  }
  script_str += "try:\n";
  script_str += "    client = _vfs_cls(address)\n";
  script_str += "    raw = client.list_directory(path)\n";
  script_str +=
      "    _result = {'success': True, 'entries': [{'name': n, 'is_directory': d, 'size': s, "
      "'last_modification_time': m, 'is_hidden': h} for n, d, s, m, h in raw]}\n";
  script_str += "\nexcept Exception as ex:\n";
  script_str += "    _result = {'success': False, 'error_message': repr(ex)}\n";

  std::unique_ptr<IDProperty, bke::idprop::IDPropertyDeleter> locals = bke::idprop::create_group(
      "locals");
  IDP_AddToGroup(locals.get(), IDP_NewString(path.endpoint.c_str(), "address"));
  IDP_AddToGroup(locals.get(), IDP_NewString(path.path.c_str(), "path"));

  std::optional<IDProperty *> idprop_opt = BPY_run_string_exec_with_locals_return_idprop(
      nullptr, script_str.c_str(), *locals, "_result");

  if (!idprop_opt.has_value()) {
    CLOG_WARN(&LOG, "Python script execution failed");
    result.success = false;
    result.error_message = "Python script execution failed";
    return result;
  }

  IDProperty *result_prop = *idprop_opt;
  if (!result_prop) {
    CLOG_WARN(&LOG, "Python script returned None");
    result.success = false;
    result.error_message = "Python script returned None";
    return result;
  }
  BLI_SCOPED_DEFER([&] { IDP_FreeProperty(result_prop); });

  if (result_prop->type != IDP_GROUP) {
    CLOG_WARN(&LOG, "Unexpected return type from Python: %d", result_prop->type);
    result.success = false;
    result.error_message = "Unexpected return type from Python";
    return result;
  }

  IDProperty *success_prop = IDP_GetPropertyFromGroup(result_prop, "success");
  if (!success_prop || success_prop->type != IDP_BOOLEAN) {
    CLOG_WARN(&LOG, "Missing 'success' field from Python result");
    CLOG_WARN(&LOG, "  result_prop->len=%d, name='%s'", result_prop->len, result_prop->name);
    for (IDProperty *child = static_cast<IDProperty *>(result_prop->data.group.first); child;
         child = child->next)
    {
      CLOG_WARN(&LOG, "  child name='%s' type=%d", child->name, child->type);
    }
    result.success = false;
    result.error_message = "Missing 'success' field from Python result";
    return result;
  }
  result.success = IDP_bool_get(success_prop);
  CLOG_INFO(&LOG, "Python script success=%d", result.success);

  if (!result.success) {
    IDProperty *err_prop = IDP_GetPropertyFromGroup(result_prop, "error_message");
    if (err_prop && err_prop->type == IDP_STRING) {
      result.error_message = IDP_string_get(err_prop);
    }
    CLOG_WARN(&LOG, "WebDAV error: %s", result.error_message.c_str());
    return result;
  }

  IDProperty *entries_prop = IDP_GetPropertyFromGroup(result_prop, "entries");
  if (!entries_prop || entries_prop->type != IDP_IDPARRAY) {
    CLOG_WARN(&LOG, "Missing 'entries' array from Python result");
    result.success = false;
    result.error_message = "Missing 'entries' array from Python result";
    return result;
  }

  result.value.emplace();
  IDProperty *array = IDP_property_array_get(entries_prop);
  for (int i = 0; i < entries_prop->len; i++) {
    IDProperty *item = &array[i];
    if (!item || item->type != IDP_GROUP) {
      continue;
    }

    IDProperty *name_prop = IDP_GetPropertyFromGroup(item, "name");
    IDProperty *is_dir_prop = IDP_GetPropertyFromGroup(item, "is_directory");
    if (!name_prop || name_prop->type != IDP_STRING) {
      continue;
    }

    VFSEntry e;
    e.name = IDP_string_get(name_prop);
    bool is_dir = (is_dir_prop && is_dir_prop->type == IDP_BOOLEAN && IDP_bool_get(is_dir_prop));
    e.is_directory = is_dir;
    if (is_dir) {
      e.flags |= VFSEntryFlags::IsDirectory;
    }

    IDProperty *is_hidden_prop = IDP_GetPropertyFromGroup(item, "is_hidden");
    if (is_hidden_prop && is_hidden_prop->type == IDP_BOOLEAN && IDP_bool_get(is_hidden_prop)) {
      e.flags |= VFSEntryFlags::IsHidden;
    }

    /* Stored as IDP_DOUBLE because Python int would map to IDP_INT (32-bit),
     * which truncates files >2 GiB and overflows epoch seconds after 2038. */
    IDProperty *size_prop = IDP_GetPropertyFromGroup(item, "size");
    if (size_prop && size_prop->type == IDP_DOUBLE) {
      e.size = uint64_t(IDP_double_get(size_prop));
    }
    IDProperty *mtime_prop = IDP_GetPropertyFromGroup(item, "last_modification_time");
    if (mtime_prop && mtime_prop->type == IDP_DOUBLE) {
      e.last_modification_time = int64_t(IDP_double_get(mtime_prop));
    }

    result.value->push_back(std::move(e));
  }

  CLOG_INFO(&LOG, "Listed %zu entries from WebDAV", result.value->size());
  return result;
#else
  (void)path;
  result.success = false;
  result.error_message = "Blender built without Python support";
  return result;
#endif
}

VFSResult<bool> PythonBackend::create_directory(const VFSPath &path) const
{
  VFSResult<bool> result;
  if (path.path.empty()) {
    result.success = false;
    result.error_message = "Empty virtual path";
    return result;
  }

#ifdef WITH_PYTHON
  std::string mod;
  std::string cls;
  parse_qualified_class_name(class_name_, mod, cls);

  /* Build the import string dynamically. */
  std::string script_str;
  if (mod.empty()) {
    script_str = "from " + cls + " import VFSWebDAV as _vfs_cls\n";
  }
  else {
    script_str = "from " + mod + " import " + cls + " as _vfs_cls\n";
  }
  script_str += "try:\n";
  script_str += "    client = _vfs_cls(address)\n";
  script_str += "    success = client.create_directory(path)\n";
  script_str += "    _result = {'success': True, 'message': 'Directory created'}\n";
  script_str += "\nexcept Exception as ex:\n";
  script_str += "    _result = {'success': False, 'error_message': repr(ex)}\n";

  std::unique_ptr<IDProperty, bke::idprop::IDPropertyDeleter> locals = bke::idprop::create_group(
      "locals");
  IDP_AddToGroup(locals.get(), IDP_NewString(path.endpoint.c_str(), "address"));
  IDP_AddToGroup(locals.get(), IDP_NewString(path.path.c_str(), "path"));

  std::optional<IDProperty *> idprop_opt = BPY_run_string_exec_with_locals_return_idprop(
      nullptr, script_str.c_str(), *locals, "_result");

  if (!idprop_opt.has_value()) {
    CLOG_WARN(&LOG, "Python script execution failed");
    result.success = false;
    result.error_message = "Python script execution failed";
    return result;
  }

  IDProperty *result_prop = *idprop_opt;
  if (!result_prop) {
    CLOG_WARN(&LOG, "Python script returned None");
    result.success = false;
    result.error_message = "Python script returned None";
    return result;
  }
  BLI_SCOPED_DEFER([&] { IDP_FreeProperty(result_prop); });

  if (result_prop->type != IDP_GROUP) {
    CLOG_WARN(&LOG, "Unexpected return type from Python: %d", result_prop->type);
    result.success = false;
    result.error_message = "Unexpected return type from Python";
    return result;
  }

  IDProperty *success_prop = IDP_GetPropertyFromGroup(result_prop, "success");
  if (!success_prop || success_prop->type != IDP_BOOLEAN) {
    CLOG_WARN(&LOG, "Missing 'success' field from Python result");
    CLOG_WARN(&LOG, "  result_prop->len=%d, name='%s'", result_prop->len, result_prop->name);
    for (IDProperty *child = static_cast<IDProperty *>(result_prop->data.group.first); child;
         child = child->next)
    {
      CLOG_WARN(&LOG, "  child name='%s' type=%d", child->name, child->type);
    }
    result.success = false;
    result.error_message = "Missing 'success' field from Python result";
    return result;
  }
  result.success = IDP_bool_get(success_prop);
  result.value = IDP_bool_get(success_prop);
  CLOG_INFO(&LOG, "Python script success=%d", result.value.value());

  if (!result.success) {
    IDProperty *err_prop = IDP_GetPropertyFromGroup(result_prop, "error_message");
    if (err_prop && err_prop->type == IDP_STRING) {
      result.error_message = IDP_string_get(err_prop);
    }
    CLOG_WARN(&LOG, "WebDAV error: %s", result.error_message.c_str());
  }

  return result;
#else
  (void)path;
  result.success = false;
  result.error_message = "Blender built without Python support";
  return result;
#endif
}

VFSResult<bool> PythonBackend::rename_item(const VFSPath &src, const VFSPath &dst) const
{
  VFSResult<bool> result;
  if (src.path.empty() || dst.path.empty()) {
    result.success = false;
    result.error_message = "Empty virtual path";
    return result;
  }

#ifdef WITH_PYTHON
  std::string mod;
  std::string cls;
  parse_qualified_class_name(class_name_, mod, cls);

  /* Build the import string dynamically. */
  std::string script_str;
  if (mod.empty()) {
    script_str = "from " + cls + " import VFSWebDAV as _vfs_cls\n";
  }
  else {
    script_str = "from " + mod + " import " + cls + " as _vfs_cls\n";
  }
  script_str += "try:\n";
  script_str += "    client = _vfs_cls(address)\n";
  script_str += "    success = client.rename_item(src, dst)\n";
  script_str += "    _result = {'success': True, 'message': 'Item renamed'}\n";
  script_str += "\nexcept Exception as ex:\n";
  script_str += "    _result = {'success': False, 'error_message': repr(ex)}\n";

  std::unique_ptr<IDProperty, bke::idprop::IDPropertyDeleter> locals = bke::idprop::create_group(
      "locals");
  IDP_AddToGroup(locals.get(), IDP_NewString(src.endpoint.c_str(), "address"));
  IDP_AddToGroup(locals.get(), IDP_NewString(src.path.c_str(), "src"));
  IDP_AddToGroup(locals.get(), IDP_NewString(dst.path.c_str(), "dst"));

  std::optional<IDProperty *> idprop_opt = BPY_run_string_exec_with_locals_return_idprop(
      nullptr, script_str.c_str(), *locals, "_result");

  if (!idprop_opt.has_value()) {
    CLOG_WARN(&LOG, "Python script execution failed");
    result.success = false;
    result.error_message = "Python script execution failed";
    return result;
  }

  IDProperty *result_prop = *idprop_opt;
  if (!result_prop) {
    CLOG_WARN(&LOG, "Python script returned None");
    result.success = false;
    result.error_message = "Python script returned None";
    return result;
  }
  BLI_SCOPED_DEFER([&] { IDP_FreeProperty(result_prop); });

  if (result_prop->type != IDP_GROUP) {
    CLOG_WARN(&LOG, "Unexpected return type from Python: %d", result_prop->type);
    result.success = false;
    result.error_message = "Unexpected return type from Python";
    return result;
  }

  IDProperty *success_prop = IDP_GetPropertyFromGroup(result_prop, "success");
  if (!success_prop || success_prop->type != IDP_BOOLEAN) {
    CLOG_WARN(&LOG, "Missing 'success' field from Python result");
    CLOG_WARN(&LOG, "  result_prop->len=%d, name='%s'", result_prop->len, result_prop->name);
    for (IDProperty *child = static_cast<IDProperty *>(result_prop->data.group.first); child;
         child = child->next)
    {
      CLOG_WARN(&LOG, "  child name='%s' type=%d", child->name, child->type);
    }
    result.success = false;
    result.error_message = "Missing 'success' field from Python result";
    return result;
  }
  result.success = IDP_bool_get(success_prop);
  result.value = IDP_bool_get(success_prop);
  CLOG_INFO(&LOG, "Python script success=%d", result.value.value());

  if (!result.success) {
    IDProperty *err_prop = IDP_GetPropertyFromGroup(result_prop, "error_message");
    if (err_prop && err_prop->type == IDP_STRING) {
      result.error_message = IDP_string_get(err_prop);
    }
    CLOG_WARN(&LOG, "WebDAV error: %s", result.error_message.c_str());
  }

  return result;
#else
  (void)src;
  (void)dst;
  result.success = false;
  result.error_message = "Blender built without Python support";
  return result;
#endif
}

VFSResult<bool> PythonBackend::exists(const VFSPath &path) const
{
  VFSResult<bool> result;
  if (path.path.empty()) {
    result.success = false;
    result.error_message = "Empty virtual path";
    return result;
  }

#ifdef WITH_PYTHON
  std::string mod;
  std::string cls;
  parse_qualified_class_name(class_name_, mod, cls);

  std::string script_str;
  if (mod.empty()) {
    script_str = "from " + cls + " import VFSWebDAV as _vfs_cls\n";
  }
  else {
    script_str = "from " + mod + " import " + cls + " as _vfs_cls\n";
  }
  script_str += "try:\n";
  script_str += "    client = _vfs_cls(address)\n";
  script_str += "    exists = client.exists(path)\n";
  script_str += "    _result = {'success': True, 'exists': exists}\n";
  script_str += "\nexcept Exception as ex:\n";
  script_str += "    _result = {'success': False, 'exists': False, 'error_message': repr(ex)}\n";

  std::unique_ptr<IDProperty, bke::idprop::IDPropertyDeleter> locals = bke::idprop::create_group(
      "locals");
  IDP_AddToGroup(locals.get(), IDP_NewString(path.endpoint.c_str(), "address"));
  IDP_AddToGroup(locals.get(), IDP_NewString(path.path.c_str(), "path"));

  std::optional<IDProperty *> idprop_opt = BPY_run_string_exec_with_locals_return_idprop(
      nullptr, script_str.c_str(), *locals, "_result");

  if (!idprop_opt.has_value()) {
    CLOG_WARN(&LOG, "Python script execution failed");
    result.success = false;
    result.error_message = "Python script execution failed";
    return result;
  }

  IDProperty *result_prop = *idprop_opt;
  if (!result_prop) {
    CLOG_WARN(&LOG, "Python script returned None");
    result.success = false;
    result.error_message = "Python script returned None";
    return result;
  }
  BLI_SCOPED_DEFER([&] { IDP_FreeProperty(result_prop); });

  if (result_prop->type != IDP_GROUP) {
    CLOG_WARN(&LOG, "Unexpected return type from Python: %d", result_prop->type);
    result.success = false;
    result.error_message = "Unexpected return type from Python";
    return result;
  }

  IDProperty *success_prop = IDP_GetPropertyFromGroup(result_prop, "success");
  if (!success_prop || success_prop->type != IDP_BOOLEAN) {
    CLOG_WARN(&LOG, "Missing 'success' field from Python result");
    result.success = false;
    result.error_message = "Missing 'success' field from Python result";
    return result;
  }
  result.success = IDP_bool_get(success_prop);
  CLOG_INFO(&LOG, "Python script success=%d", result.success);

  if (!result.success) {
    IDProperty *err_prop = IDP_GetPropertyFromGroup(result_prop, "error_message");
    if (err_prop && err_prop->type == IDP_STRING) {
      result.error_message = IDP_string_get(err_prop);
    }
    CLOG_WARN(&LOG, "WebDAV error: %s", result.error_message.c_str());
    return result;
  }

  IDProperty *exists_prop = IDP_GetPropertyFromGroup(result_prop, "exists");
  if (!exists_prop || exists_prop->type != IDP_BOOLEAN) {
    CLOG_WARN(&LOG, "Missing 'exists' field from Python result");
    result.success = false;
    result.error_message = "Missing 'exists' field from Python result";
    return result;
  }
  result.value = IDP_bool_get(exists_prop);
  CLOG_INFO(&LOG, "Python script exists=%d", result.value.value());

  return result;
#else
  (void)path;
  result.success = false;
  result.error_message = "Blender built without Python support";
  return result;
#endif
}

VFSResult<bool> PythonBackend::delete_item(const VFSPath &path) const
{
  VFSResult<bool> result;
  if (path.path.empty()) {
    result.success = false;
    result.error_message = "Empty virtual path";
    return result;
  }

#ifdef WITH_PYTHON
  std::string mod;
  std::string cls;
  parse_qualified_class_name(class_name_, mod, cls);

  std::string script_str;
  if (mod.empty()) {
    script_str = "from " + cls + " import VFSWebDAV as _vfs_cls\n";
  }
  else {
    script_str = "from " + mod + " import " + cls + " as _vfs_cls\n";
  }
  script_str += "try:\n";
  script_str += "    client = _vfs_cls(address)\n";
  script_str += "    success = client.delete_item(path)\n";
  script_str += "    _result = {'success': True, 'deleted': success}\n";
  script_str += "\nexcept Exception as ex:\n";
  script_str += "    _result = {'success': False, 'deleted': False, 'error_message': repr(ex)}\n";

  std::unique_ptr<IDProperty, bke::idprop::IDPropertyDeleter> locals = bke::idprop::create_group(
      "locals");
  IDP_AddToGroup(locals.get(), IDP_NewString(path.endpoint.c_str(), "address"));
  IDP_AddToGroup(locals.get(), IDP_NewString(path.path.c_str(), "path"));

  std::optional<IDProperty *> idprop_opt = BPY_run_string_exec_with_locals_return_idprop(
      nullptr, script_str.c_str(), *locals, "_result");

  if (!idprop_opt.has_value()) {
    CLOG_WARN(&LOG, "Python script execution failed");
    result.success = false;
    result.error_message = "Python script execution failed";
    return result;
  }

  IDProperty *result_prop = *idprop_opt;
  if (!result_prop) {
    CLOG_WARN(&LOG, "Python script returned None");
    result.success = false;
    result.error_message = "Python script returned None";
    return result;
  }
  BLI_SCOPED_DEFER([&] { IDP_FreeProperty(result_prop); });

  if (result_prop->type != IDP_GROUP) {
    CLOG_WARN(&LOG, "Unexpected return type from Python: %d", result_prop->type);
    result.success = false;
    result.error_message = "Unexpected return type from Python";
    return result;
  }

  IDProperty *success_prop = IDP_GetPropertyFromGroup(result_prop, "success");
  if (!success_prop || success_prop->type != IDP_BOOLEAN) {
    CLOG_WARN(&LOG, "Missing 'success' field from Python result");
    result.success = false;
    result.error_message = "Missing 'success' field from Python result";
    return result;
  }
  result.success = IDP_bool_get(success_prop);
  CLOG_INFO(&LOG, "Python script success=%d", result.success);

  if (!result.success) {
    IDProperty *err_prop = IDP_GetPropertyFromGroup(result_prop, "error_message");
    if (err_prop && err_prop->type == IDP_STRING) {
      result.error_message = IDP_string_get(err_prop);
    }
    CLOG_WARN(&LOG, "WebDAV error: %s", result.error_message.c_str());
    return result;
  }

  IDProperty *deleted_prop = IDP_GetPropertyFromGroup(result_prop, "deleted");
  if (!deleted_prop || deleted_prop->type != IDP_BOOLEAN) {
    CLOG_WARN(&LOG, "Missing 'deleted' field from Python result");
    result.success = false;
    result.error_message = "Missing 'deleted' field from Python result";
    return result;
  }
  result.value = IDP_bool_get(deleted_prop);
  CLOG_INFO(&LOG, "Python script deleted=%d", result.value.value());

  return result;
#else
  (void)path;
  result.success = false;
  result.error_message = "Blender built without Python support";
  return result;
#endif
}

std::unique_ptr<VFSBackend> get_python_wrapper(const char *class_name) noexcept
{
#ifdef WITH_PYTHON
  return std::make_unique<PythonBackend>(class_name);
#else
  UNUSED(class_name);
  return nullptr;
#endif
}

}  // namespace blender::vfs
