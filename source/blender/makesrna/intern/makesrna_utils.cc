/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <sstream>

#include "makesrna_utils.hh"

#include "BLI_string.h"

void rna_write_struct_forward_declarations(std::ostringstream &stream,
                                           blender::VectorSet<std::string> &&structs_set)
{
  blender::Vector<std::string> structs_vec = structs_set.extract_vector();
  std::stable_sort(structs_vec.begin(),
                   structs_vec.end(),
                   [](const blender::StringRef a, const blender::StringRef b) {
                     /* Keep structs within namespaces last. */
                     bool scoped_name[2] = {a.find("::") != blender::StringRef::not_found,
                                            b.find("::") != blender::StringRef::not_found};
                     return (scoped_name[0] == scoped_name[1] &&
                             BLI_strcasecmp(a.data(), b.data()) < 0) ||
                            scoped_name[0] < scoped_name[1];
                   });

  /* For grouping structs within namespaces. */
  blender::StringRef last_namespace = "";

  for (const blender::StringRef type : structs_vec) {
    blender::StringRef namespace_name;
    blender::StringRef struct_name;
    int namespace_length = type.find_last_of("::");
    if (namespace_length != blender::StringRef::not_found) {
      namespace_name = type.substr(0, namespace_length - 1);
      struct_name = type.substr(namespace_length + 1);
    }
    else {
      struct_name = type;
    }
    if (namespace_name != last_namespace && !last_namespace.is_empty()) {
      stream << "}; // namespace " << std::string_view(last_namespace) << '\n';
    }
    if (namespace_name != last_namespace && !namespace_name.is_empty()) {
      stream << "namespace " << std::string_view(namespace_name) << " {\n";
    }
    stream << "struct " << std::string_view(struct_name) << ";\n";
    last_namespace = namespace_name;
  }
  if (!last_namespace.is_empty()) {
    stream << "}; // namespace " << std::string_view(last_namespace) << '\n';
  }
}
