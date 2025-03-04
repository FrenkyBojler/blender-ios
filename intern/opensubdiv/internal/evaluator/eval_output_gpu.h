/* SPDX-FileCopyrightText: 2021 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Author: Sergey Sharybin. */

#ifndef OPENSUBDIV_EVAL_OUTPUT_GPU_H_
#define OPENSUBDIV_EVAL_OUTPUT_GPU_H_

#include "internal/evaluator/eval_output.h"
#include "internal/evaluator/gpu_compute_evaluator.h"
#include "internal/evaluator/gpu_patch_table.hh"

#include <opensubdiv/osd/glPatchTable.h>
#include <opensubdiv/osd/glVertexBuffer.h>

#include "gpu_vertex_buffer.hh"

namespace blender::opensubdiv {

class GpuEvalOutput : public VolatileEvalOutput<GPUVertexBuffer,
                                                GPUVertexBuffer,
                                                GPUStencilTableSSBO,
                                                GPUPatchTable,
                                                GPUComputeEvaluator> {
 public:
  GpuEvalOutput(const StencilTable *vertex_stencils,
                const StencilTable *varying_stencils,
                const std::vector<const StencilTable *> &all_face_varying_stencils,
                const int face_varying_width,
                const PatchTable *patch_table,
                EvaluatorCache *evaluator_cache = nullptr);

  void fillPatchArraysBuffer(blender::gpu::VertBuf *patch_arrays_buffer) override;

  gpu::VertBuf *wrapPatchIndexBuffer() override
  {
    return getPatchTable()->GetPatchIndexBuffer();
  }

  gpu::VertBuf *wrapPatchParamBuffer() override
  {
    return getPatchTable()->GetPatchParamBuffer();
  }

  gpu::VertBuf *wrapSrcBuffer() override
  {
    return getSrcBuffer()->get_vertex_buffer();
  }

  gpu::VertBuf *wrapSrcVertexDataBuffer() override
  {
    return getSrcVertexDataBuffer()->get_vertex_buffer();
  }

  void fillFVarPatchArraysBuffer(const int face_varying_channel,
                                 blender::gpu::VertBuf *patch_arrays_buffer) override;

  gpu::VertBuf *wrapFVarPatchIndexBuffer(const int face_varying_channel) override
  {
    GPUPatchTable *patch_table = getFVarPatchTable(face_varying_channel);
    return patch_table->GetFVarPatchIndexBuffer(face_varying_channel);
  }

  gpu::VertBuf *wrapFVarPatchParamBuffer(const int face_varying_channel) override
  {
    GPUPatchTable *patch_table = getFVarPatchTable(face_varying_channel);
    return patch_table->GetFVarPatchParamBuffer(face_varying_channel);
  }

  gpu::VertBuf *wrapFVarSrcBuffer(const int face_varying_channel) override
  {
    GPUVertexBuffer *vertex_buffer = getFVarSrcBuffer(face_varying_channel);
    return vertex_buffer->get_vertex_buffer();
  }
};

}  // namespace blender::opensubdiv

#endif  // OPENSUBDIV_EVAL_OUTPUT_GPU_H_
