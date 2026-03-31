/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_resource_scope.hh"

#include "RNA_types.hh"

namespace blender {

struct StructRNA;
struct bNodeTree;

namespace nodes {

struct GeneratedTreeSrnaData {
  ResourceScope scope;
  StructRNA *properties_struct;
  BlenderRNA *generated_rna;
  GeneratedTreeSrnaData();
  ~GeneratedTreeSrnaData();
};

}  // namespace nodes
}  // namespace blender
