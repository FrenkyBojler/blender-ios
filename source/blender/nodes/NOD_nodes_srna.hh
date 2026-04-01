/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_resource_scope.hh"

#include "RNA_define.hh"

namespace blender {

struct BlenderRNA;
struct StructRNA;
namespace nodes {

struct GeneratedTreeSrnaData {
  ResourceScope scope;
  StructRNA *properties_struct;
  BlenderRNA *generated_rna;
  GeneratedTreeSrnaData()
  {
    generated_rna = RNA_create_runtime();
  }
  ~GeneratedTreeSrnaData()
  {
    RNA_free(generated_rna);
  }
};

}  // namespace nodes
}  // namespace blender
