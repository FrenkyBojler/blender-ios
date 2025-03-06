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

static void discard_buffer(GPUStorageBuf **buffer)
{
  if (*buffer != nullptr) {
    GPU_storagebuf_free(*buffer);
    *buffer = nullptr;
  }
}

static void discard_list(std::vector<GPUStorageBuf *> &buffers)
{
  while (!buffers.empty()) {
    GPUStorageBuf *buffer = buffers.back();
    buffers.pop_back();
    GPU_storagebuf_free(buffer);
  }
}

GPUPatchTable::~GPUPatchTable()
{
  discard_buffer(&_patchIndexBuffer);
  discard_buffer(&_patchParamBuffer);
  discard_buffer(&_varyingIndexBuffer);
  discard_list(_fvarIndexBuffers);
  discard_list(_fvarParamBuffers);
}

bool GPUPatchTable::allocate(PatchTable const *far_patch_table)
{

  return false;
}

}  // namespace blender::opensubdiv
