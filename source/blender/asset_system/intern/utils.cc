/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup asset_system
 */

/* For PATH_MAX (at least on Windows). */
#include "BLI_fileops.hh"  // IWYU pragma: keep
#include "BLI_path_utils.hh"
#include "BLI_string.hh"

#include "MEM_guardedalloc.h"

#include "BKE_global.hh"
#include "BKE_path_templates.hh"

#include "utils.hh"

namespace blender::asset_system::utils {

namespace path_templates = bke::path_templates;

std::string normalize_directory_path(StringRef directory)
{
  if (directory.is_empty()) {
    return "";
  }
  char dir_normalized[PATH_MAX];
  BLI_strncpy(dir_normalized,
              directory.data(),
              /* + 1 for null terminator. */
              std::min(directory.size() + 1, int64_t(sizeof(dir_normalized))));

  path_templates::VariableMap template_variables;
  bke::BlenderProject *project = BKE_blender_project_get(G_MAIN);
  BKE_add_template_variables_general(template_variables, nullptr, project);
  const Vector<path_templates::Error> variable_errors = BKE_path_apply_template(
      dir_normalized, FILE_MAX, template_variables);
  if (!variable_errors.is_empty()) {
    // return variable_errors;
  }
  BLI_path_slash_native(dir_normalized);
  BLI_path_normalize_dir(dir_normalized, sizeof(dir_normalized));
  return std::string(dir_normalized);
}

std::string normalize_path(StringRefNull path, int64_t max_len)
{
  const int64_t len = (max_len == StringRef::not_found) ? path.size() :
                                                          std::min(max_len, path.size());

  char *buf = BLI_strdupn(path.c_str(), len);

  path_templates::VariableMap template_variables;
  bke::BlenderProject *project = BKE_blender_project_get(G_MAIN);
  BKE_add_template_variables_general(template_variables, nullptr, project);
  const Vector<path_templates::Error> variable_errors = BKE_path_apply_template(
      buf, FILE_MAX, template_variables);
  if (!variable_errors.is_empty()) {
    // return variable_errors;
  }

  BLI_path_slash_native(buf);
  BLI_path_normalize(buf);

  std::string normalized_path = buf;
  MEM_delete(buf);

  if (len != path.size()) {
    normalized_path = normalized_path + path.substr(len);
  }

  return normalized_path;
}

}  // namespace blender::asset_system::utils
