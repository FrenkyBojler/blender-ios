/* SPDX-FileCopyrightText: 2025 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "gpu_patch_table.hh"

#include "opensubdiv/far/patchTable.h"
#include "opensubdiv/osd/cpuPatchTable.h"

namespace blender::opensubdiv {


GPUPatchTable *GPUPatchTable::Create(PatchTable const *far_patch_table, void * /*deviceContext*/)
{
  GPUPatchTable *instance = new GPUPatchTable();
  if (instance->allocate(far_patch_table))
    return instance;
  delete instance;
  return nullptr;
}

GPUPatchTable::~GPUPatchTable() {}

bool GPUPatchTable::allocate(PatchTable const *far_patch_table)
{
  return false;
}

}  // namespace blender::opensubdiv
