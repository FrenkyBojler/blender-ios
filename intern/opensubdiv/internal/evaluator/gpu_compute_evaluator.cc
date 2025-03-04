/* SPDX-FileCopyrightText: 2015 Pixar
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <epoxy/gl.h>

/* There are few aspects here:
 *   - macOS is strict about including both gl.h and gl3.h
 *   - libepoxy only pretends to be a replacement for gl.h
 *   - OpenSubdiv internally uses `OpenGL/gl3.h` on macOS
 *
 * In order to silence the warning pretend that gl3 has been included, fully relying on symbols
 * from the epoxy.
 *
 * This works differently from how OpenSubdiv internally will use `OpenGL/gl3.h` without epoxy.
 * Sounds fragile, but so far things seems to work. */
#if defined(__APPLE__)
#  define __gl3_h_
#endif

#include "gpu_compute_evaluator.h"

#include <opensubdiv/far/error.h>
#include <opensubdiv/far/patchDescriptor.h>
#include <opensubdiv/far/stencilTable.h>
#include <opensubdiv/osd/glslPatchShaderSource.h>

#include <cassert>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

#include "GPU_capabilities.hh"
#include "GPU_compute.hh"
#include "GPU_context.hh"
#include "GPU_state.hh"
#include "GPU_vertex_buffer.hh"
#include "gpu_shader_create_info.hh"

using OpenSubdiv::Far::LimitStencilTable;
using OpenSubdiv::Far::StencilTable;
using OpenSubdiv::Osd::BufferDescriptor;
using OpenSubdiv::Osd::PatchArray;
using OpenSubdiv::Osd::PatchArrayVector;

#define SHADER_SRC_VERTEX_BUFFER_BUF_SLOT 0
#define SHADER_DST_VERTEX_BUFFER_BUF_SLOT 1
#define SHADER_DU_BUFFER_BUF_SLOT 2
#define SHADER_DV_BUFFER_BUF_SLOT 3
#define SHADER_SIZES_BUF_SLOT 4
#define SHADER_OFFSETS_BUF_SLOT 5
#define SHADER_INDICES_BUF_SLOT 6
#define SHADER_WEIGHTS_BUF_SLOT 7
#define SHADER_DU_WEIGHTS_BUF_SLOT 8
#define SHADER_DV_WEIGHTS_BUF_SLOT 9
#define SHADER_DUU_BUFFER_BUF_SLOT 10
#define SHADER_DUV_BUFFER_BUF_SLOT 11
#define SHADER_DVV_BUFFER_BUF_SLOT 12
#define SHADER_DUU_WEIGHTS_BUF_SLOT 13
#define SHADER_DUV_WEIGHTS_BUF_SLOT 14
#define SHADER_DVV_WEIGHTS_BUF_SLOT 15

#define SHADER_PATCH_ARRAY_BUFFER_BUF_SLOT 4
#define SHADER_PATCH_COORDS_BUF_SLOT 5
#define SHADER_PATCH_INDEX_BUFFER_BUF_SLOT 6
#define SHADER_PATCH_PARAM_BUFFER_BUF_SLOT 7

namespace blender::opensubdiv {

template<class T>
gpu::VertBuf *create_buffer(std::vector<T> const &src,
                            GPUVertCompType comp_type,
                            GPUVertFetchMode fetch_mode)
{
  if (src.empty()) {
    return nullptr;
  }
  // TODO: move outside this function.
  GPUVertFormat format = {};
  GPU_vertformat_clear(&format);
  GPU_vertformat_attr_add(&format, "data", comp_type, 1, fetch_mode);

  gpu::VertBuf *vertex_buffer = GPU_vertbuf_create_with_format(format);
  GPU_vertbuf_data_alloc(*vertex_buffer, src.size());
  GPU_vertbuf_use(vertex_buffer);
  GPU_vertbuf_update_sub(vertex_buffer, 0, src.size(), &src.at(0));

  return vertex_buffer;
}

GPUStencilTableSSBO::GPUStencilTableSSBO(StencilTable const *stencilTable)
{
  _numStencils = stencilTable->GetNumStencils();
  if (_numStencils > 0) {
    _sizes = create_buffer(stencilTable->GetSizes(), GPU_COMP_I32, GPU_FETCH_INT);
    _offsets = create_buffer(stencilTable->GetOffsets(), GPU_COMP_I32, GPU_FETCH_INT);
    _indices = create_buffer(stencilTable->GetControlIndices(), GPU_COMP_I32, GPU_FETCH_INT);
    _weights = create_buffer(stencilTable->GetWeights(), GPU_COMP_F32, GPU_FETCH_FLOAT);
  }
}

GPUStencilTableSSBO::GPUStencilTableSSBO(LimitStencilTable const *limitStencilTable)
{
  _numStencils = limitStencilTable->GetNumStencils();
  if (_numStencils > 0) {
    _sizes = create_buffer(limitStencilTable->GetSizes(), GPU_COMP_I32, GPU_FETCH_INT);
    _offsets = create_buffer(limitStencilTable->GetOffsets(), GPU_COMP_I32, GPU_FETCH_INT);
    _indices = create_buffer(limitStencilTable->GetControlIndices(), GPU_COMP_I32, GPU_FETCH_INT);
    _weights = create_buffer(limitStencilTable->GetWeights(), GPU_COMP_F32, GPU_FETCH_FLOAT);
    _duWeights = create_buffer(limitStencilTable->GetDuWeights(), GPU_COMP_F32, GPU_FETCH_FLOAT);
    _duWeights = create_buffer(limitStencilTable->GetDuWeights(), GPU_COMP_F32, GPU_FETCH_FLOAT);
    _duuWeights = create_buffer(limitStencilTable->GetDuuWeights(), GPU_COMP_F32, GPU_FETCH_FLOAT);
    _duvWeights = create_buffer(limitStencilTable->GetDuvWeights(), GPU_COMP_F32, GPU_FETCH_FLOAT);
    _dvvWeights = create_buffer(limitStencilTable->GetDvvWeights(), GPU_COMP_F32, GPU_FETCH_FLOAT);
  }
}

GPUStencilTableSSBO::~GPUStencilTableSSBO()
{
#define SAFE_FREE_VERTEX_BUFFER(buffer) \
  if (buffer) { \
    GPU_vertbuf_discard(buffer); \
    buffer = nullptr; \
  }
  SAFE_FREE_VERTEX_BUFFER(_sizes)
  SAFE_FREE_VERTEX_BUFFER(_offsets)
  SAFE_FREE_VERTEX_BUFFER(_indices)
  SAFE_FREE_VERTEX_BUFFER(_weights)
  SAFE_FREE_VERTEX_BUFFER(_duWeights)
  SAFE_FREE_VERTEX_BUFFER(_dvWeights)
  SAFE_FREE_VERTEX_BUFFER(_duuWeights)
  SAFE_FREE_VERTEX_BUFFER(_duvWeights)
  SAFE_FREE_VERTEX_BUFFER(_dvvWeights)
#undef SAFE_FREE_SSBO
}

// ---------------------------------------------------------------------------

GPUComputeEvaluator::GPUComputeEvaluator() : _workGroupSize(64), _patchArraysSSBO(nullptr)
{
  memset((void *)&_stencilKernel, 0, sizeof(_stencilKernel));
  memset((void *)&_patchKernel, 0, sizeof(_patchKernel));
}

GPUComputeEvaluator::~GPUComputeEvaluator()
{
  if (_patchArraysSSBO) {
    GPU_storagebuf_free(_patchArraysSSBO);
    _patchArraysSSBO = nullptr;
  }
}

static GPUShader *compileKernel(BufferDescriptor const &srcDesc,
                                BufferDescriptor const &dstDesc,
                                BufferDescriptor const &duDesc,
                                BufferDescriptor const &dvDesc,
                                BufferDescriptor const &duuDesc,
                                BufferDescriptor const &duvDesc,
                                BufferDescriptor const &dvvDesc,
                                bool use_eval_stencil_kernel,
                                int workGroupSize)
{
  using namespace blender::gpu::shader;
  ShaderCreateInfo info("opensubdiv_compute_eval");
  info.local_group_size(workGroupSize, 1, 1);
  if (GPU_backend_get_type() == GPU_BACKEND_METAL) {
    info.define("OSD_PATCH_BASIS_METAL");
  }
  else {
    info.define("OSD_PATCH_BASIS_GLSL");
  }
  if (use_eval_stencil_kernel) {
    info.define("OPENSUBDIV_GLSL_COMPUTE_KERNEL_EVAL_STENCILS");
  }
  else {
    info.define("OPENSUBDIV_GLSL_COMPUTE_KERNEL_EVAL_PATCHES");
  }

  // TODO: use specialization constants for length, src_stride, dst_stride. Not sure we can use
  // work group size as that requires extensions. This allows us to compile less shaders and
  // improve overall performance. Adding length as specialization constant will not work as it is
  // used to define an array length. This is not supported by Metal.
  info.define("LENGTH", std::to_string(srcDesc.length));
  info.define("SRC_STRIDE", std::to_string(srcDesc.stride));
  info.define("DST_STRIDE", std::to_string(dstDesc.stride));
  info.define("WORK_GROUP_SIZE", std::to_string(workGroupSize));
  info.typedef_source("osd_patch_basis.glsl");
  info.storage_buf(
      SHADER_SRC_VERTEX_BUFFER_BUF_SLOT, Qualifier::READ, "float", "srcVertexBuffer[]");
  info.storage_buf(
      SHADER_DST_VERTEX_BUFFER_BUF_SLOT, Qualifier::WRITE, "float", "dstVertexBuffer[]");
  info.push_constant(Type::INT, "srcOffset");
  info.push_constant(Type::INT, "dstOffset");

  bool deriv1 = (duDesc.length > 0 || dvDesc.length > 0);
  bool deriv2 = (duuDesc.length > 0 || duvDesc.length > 0 || dvvDesc.length > 0);
  if (deriv1) {
    info.define("OPENSUBDIV_GLSL_COMPUTE_USE_1ST_DERIVATIVES");
    info.storage_buf(SHADER_DU_BUFFER_BUF_SLOT, Qualifier::WRITE, "float", "duBuffer[]");
    info.storage_buf(SHADER_DV_BUFFER_BUF_SLOT, Qualifier::WRITE, "float", "dvBuffer[]");
    info.push_constant(Type::IVEC3, "duDesc");
    info.push_constant(Type::IVEC3, "dvDesc");
  }
  if (deriv2) {
    info.define("OPENSUBDIV_GLSL_COMPUTE_USE_2ND_DERIVATIVES");
    info.storage_buf(SHADER_DUU_BUFFER_BUF_SLOT, Qualifier::WRITE, "float", "duuBuffer[]");
    info.storage_buf(SHADER_DUV_BUFFER_BUF_SLOT, Qualifier::WRITE, "float", "duvBuffer[]");
    info.storage_buf(SHADER_DVV_BUFFER_BUF_SLOT, Qualifier::WRITE, "float", "dvvBuffer[]");
    info.push_constant(Type::IVEC3, "duuDesc");
    info.push_constant(Type::IVEC3, "duvDesc");
    info.push_constant(Type::IVEC3, "dvvDesc");
  }

  if (use_eval_stencil_kernel) {
    info.storage_buf(SHADER_SIZES_BUF_SLOT, Qualifier::READ, "int", "_sizes[]");
    info.storage_buf(SHADER_OFFSETS_BUF_SLOT, Qualifier::READ, "int", "_offsets[]");
    info.storage_buf(SHADER_INDICES_BUF_SLOT, Qualifier::READ, "int", "_indices[]");
    info.storage_buf(SHADER_WEIGHTS_BUF_SLOT, Qualifier::READ, "float", "_weights[]");
    if (deriv1) {
      info.storage_buf(SHADER_DU_WEIGHTS_BUF_SLOT, Qualifier::READ, "float", "_duWeights[]");
      info.storage_buf(SHADER_DV_WEIGHTS_BUF_SLOT, Qualifier::READ, "float", "_dvWeights[]");
    }
    if (deriv2) {
      info.storage_buf(SHADER_DUU_WEIGHTS_BUF_SLOT, Qualifier::READ, "float", "_duuWeights[]");
      info.storage_buf(SHADER_DUV_WEIGHTS_BUF_SLOT, Qualifier::READ, "float", "_duvWeights[]");
      info.storage_buf(SHADER_DVV_WEIGHTS_BUF_SLOT, Qualifier::READ, "float", "_dvvWeights[]");
    }
    info.push_constant(Type::INT, "batchStart");
    info.push_constant(Type::INT, "batchEnd");
  }
  else {
    info.storage_buf(SHADER_PATCH_ARRAY_BUFFER_BUF_SLOT,
                     Qualifier::READ,
                     "OsdPatchArray",
                     "patchArrayBuffer[]");
    info.storage_buf(
        SHADER_PATCH_COORDS_BUF_SLOT, Qualifier::READ, "OsdPatchCoord", "patchCoords[]");
    info.storage_buf(
        SHADER_PATCH_INDEX_BUFFER_BUF_SLOT, Qualifier::READ, "int", "patchIndexBuffer[]");
    info.storage_buf(SHADER_PATCH_PARAM_BUFFER_BUF_SLOT,
                     Qualifier::READ,
                     "OsdPatchParam",
                     "patchParamBuffer[]");
  }
  info.compute_source("osd_kernel_comp.glsl");
  GPUShader *shader = GPU_shader_create_from_info(
      reinterpret_cast<const GPUShaderCreateInfo *>(&info));
  return shader;
}

bool GPUComputeEvaluator::Compile(BufferDescriptor const &srcDesc,
                                  BufferDescriptor const &dstDesc,
                                  BufferDescriptor const &duDesc,
                                  BufferDescriptor const &dvDesc,
                                  BufferDescriptor const &duuDesc,
                                  BufferDescriptor const &duvDesc,
                                  BufferDescriptor const &dvvDesc)
{

  // create a stencil kernel
  if (!_stencilKernel.Compile(
          srcDesc, dstDesc, duDesc, dvDesc, duuDesc, duvDesc, dvvDesc, _workGroupSize))
  {
    return false;
  }

  // create a patch kernel
  if (!_patchKernel.Compile(
          srcDesc, dstDesc, duDesc, dvDesc, duuDesc, duvDesc, dvvDesc, _workGroupSize))
  {
    return false;
  }

// create a patch arrays buffer
// TODO: unknown size....
#if 0
  if (!_patchArraysSSBO) {
    glGenBuffers(1, &_patchArraysSSBO);
  }
#endif

  return true;
}

/* static */
void GPUComputeEvaluator::Synchronize(void * /*kernel*/)
{
  // XXX: this is currently just for the performance measuring purpose.
  // need to be reimplemented by fence and sync.
  GPU_finish();
}

int GPUComputeEvaluator::GetDispatchSize(int count) const
{
  return (count + _workGroupSize - 1) / _workGroupSize;
}

void GPUComputeEvaluator::DispatchCompute(GPUShader *shader, int totalDispatchSize) const
{
  const int dispatchSize = GetDispatchSize(totalDispatchSize);
  int dispatchRX = dispatchSize;
  int dispatchRY = 1u;
  if (dispatchRX > GPU_max_work_group_count(0)) {
    /* Since there are some limitations with regards to the maximum work group size (could be as
     * low as 64k elements per call), we split the number elements into a "2d" number, with the
     * final index being computed as `res_x + res_y * max_work_group_size`. Even with a maximum
     * work group size of 64k, that still leaves us with roughly `64k * 64k = 4` billion elements
     * total, which should be enough. If not, we could also use the 3rd dimension. */
    /* TODO(fclem): We could dispatch fewer groups if we compute the prime factorization and
     * get the smallest rect fitting the requirements. */
    dispatchRX = dispatchRY = std::ceil(std::sqrt(dispatchSize));
    /* Avoid a completely empty dispatch line caused by rounding. */
    if ((dispatchRX * (dispatchRY - 1)) >= dispatchSize) {
      dispatchRY -= 1;
    }
  }

  /* X and Y dimensions may have different limits so the above computation may not be right, but
   * even with the standard 64k minimum on all dimensions we still have a lot of room. Therefore,
   * we presume it all fits. */
  assert(dispatchRY < GPU_max_work_group_count(1));
  GPU_compute_dispatch(shader, dispatchRX, dispatchRY, 1);
}

bool GPUComputeEvaluator::EvalStencils(gpu::VertBuf *srcBuffer,
                                       BufferDescriptor const &srcDesc,
                                       gpu::VertBuf *dstBuffer,
                                       BufferDescriptor const &dstDesc,
                                       gpu::VertBuf *duBuffer,
                                       BufferDescriptor const &duDesc,
                                       gpu::VertBuf *dvBuffer,
                                       BufferDescriptor const &dvDesc,
                                       gpu::VertBuf *sizesBuffer,
                                       gpu::VertBuf *offsetsBuffer,
                                       gpu::VertBuf *indicesBuffer,
                                       gpu::VertBuf *weightsBuffer,
                                       gpu::VertBuf *duWeightsBuffer,
                                       gpu::VertBuf *dvWeightsBuffer,
                                       int start,
                                       int end) const
{

  return EvalStencils(srcBuffer,
                      srcDesc,
                      dstBuffer,
                      dstDesc,
                      duBuffer,
                      duDesc,
                      dvBuffer,
                      dvDesc,
                      nullptr,
                      BufferDescriptor(),
                      nullptr,
                      BufferDescriptor(),
                      nullptr,
                      BufferDescriptor(),
                      sizesBuffer,
                      offsetsBuffer,
                      indicesBuffer,
                      weightsBuffer,
                      duWeightsBuffer,
                      dvWeightsBuffer,
                      nullptr,
                      nullptr,
                      nullptr,
                      start,
                      end);
}

bool GPUComputeEvaluator::EvalStencils(gpu::VertBuf *srcBuffer,
                                       BufferDescriptor const &srcDesc,
                                       gpu::VertBuf *dstBuffer,
                                       BufferDescriptor const &dstDesc,
                                       gpu::VertBuf *duBuffer,
                                       BufferDescriptor const &duDesc,
                                       gpu::VertBuf *dvBuffer,
                                       BufferDescriptor const &dvDesc,
                                       gpu::VertBuf *duuBuffer,
                                       BufferDescriptor const &duuDesc,
                                       gpu::VertBuf *duvBuffer,
                                       BufferDescriptor const &duvDesc,
                                       gpu::VertBuf *dvvBuffer,
                                       BufferDescriptor const &dvvDesc,
                                       gpu::VertBuf *sizesBuffer,
                                       gpu::VertBuf *offsetsBuffer,
                                       gpu::VertBuf *indicesBuffer,
                                       gpu::VertBuf *weightsBuffer,
                                       gpu::VertBuf *duWeightsBuffer,
                                       gpu::VertBuf *dvWeightsBuffer,
                                       gpu::VertBuf *duuWeightsBuffer,
                                       gpu::VertBuf *duvWeightsBuffer,
                                       gpu::VertBuf *dvvWeightsBuffer,
                                       int start,
                                       int end) const
{

  if (_stencilKernel.shader == nullptr) {
    return false;
  }
  int count = end - start;
  if (count <= 0) {
    return true;
  }

  GPU_shader_bind(_stencilKernel.shader);
  GPU_vertbuf_bind_as_ssbo(srcBuffer, SHADER_SRC_VERTEX_BUFFER_BUF_SLOT);
  GPU_vertbuf_bind_as_ssbo(dstBuffer, SHADER_DST_VERTEX_BUFFER_BUF_SLOT);
  GPU_vertbuf_bind_as_ssbo(duBuffer, SHADER_DU_BUFFER_BUF_SLOT);
  GPU_vertbuf_bind_as_ssbo(dvBuffer, SHADER_DV_BUFFER_BUF_SLOT);
  if (duuBuffer) {
    GPU_vertbuf_bind_as_ssbo(duuBuffer, SHADER_DUU_BUFFER_BUF_SLOT);
  }
  if (duvBuffer) {
    GPU_vertbuf_bind_as_ssbo(duvBuffer, SHADER_DUV_BUFFER_BUF_SLOT);
  }
  if (dvvBuffer) {
    GPU_vertbuf_bind_as_ssbo(dvvBuffer, SHADER_DVV_BUFFER_BUF_SLOT);
  }
  GPU_vertbuf_bind_as_ssbo(sizesBuffer, SHADER_SIZES_BUF_SLOT);
  GPU_vertbuf_bind_as_ssbo(offsetsBuffer, SHADER_OFFSETS_BUF_SLOT);
  GPU_vertbuf_bind_as_ssbo(indicesBuffer, SHADER_INDICES_BUF_SLOT);
  GPU_vertbuf_bind_as_ssbo(weightsBuffer, SHADER_WEIGHTS_BUF_SLOT);
  if (duWeightsBuffer) {
    GPU_vertbuf_bind_as_ssbo(duWeightsBuffer, SHADER_DU_WEIGHTS_BUF_SLOT);
  }
  if (dvWeightsBuffer) {
    GPU_vertbuf_bind_as_ssbo(dvWeightsBuffer, SHADER_DV_WEIGHTS_BUF_SLOT);
  }
  if (duuWeightsBuffer) {
    GPU_vertbuf_bind_as_ssbo(duuWeightsBuffer, SHADER_DUU_WEIGHTS_BUF_SLOT);
  }
  if (duvWeightsBuffer) {
    GPU_vertbuf_bind_as_ssbo(duvWeightsBuffer, SHADER_DUV_WEIGHTS_BUF_SLOT);
  }
  if (dvvWeightsBuffer) {
    GPU_vertbuf_bind_as_ssbo(dvvWeightsBuffer, SHADER_DVV_WEIGHTS_BUF_SLOT);
  }

  GPU_shader_uniform_int_ex(_stencilKernel.shader, _stencilKernel.uniformStart, 1, 0, &start);
  GPU_shader_uniform_int_ex(_stencilKernel.shader, _stencilKernel.uniformEnd, 1, 0, &end);
  GPU_shader_uniform_int_ex(
      _stencilKernel.shader, _stencilKernel.uniformSrcOffset, 1, 0, &srcDesc.offset);
  GPU_shader_uniform_int_ex(
      _stencilKernel.shader, _stencilKernel.uniformDstOffset, 1, 0, &dstDesc.offset);

// TODO init to -1 and check >= 0 to align with GPU module. Currently we assume that the uniform
// location is not zero as there are other uniforms defined as well.
#define BIND_BUF_DESC(uniform, desc) \
  if (_stencilKernel.uniform > 0) { \
    int value[] = {desc.offset, desc.length, desc.stride}; \
    GPU_shader_uniform_int_ex(_stencilKernel.shader, _stencilKernel.uniform, 3, 0, value); \
  }
  BIND_BUF_DESC(uniformDuDesc, duDesc)
  BIND_BUF_DESC(uniformDvDesc, dvDesc)
  BIND_BUF_DESC(uniformDuuDesc, duuDesc)
  BIND_BUF_DESC(uniformDuvDesc, duvDesc)
  BIND_BUF_DESC(uniformDvvDesc, dvvDesc)
#undef BIND_BUF_DESC
  DispatchCompute(_stencilKernel.shader, count);
  // GPU_storagebuf_unbind_all();
  GPU_shader_unbind();

  return true;
}

bool GPUComputeEvaluator::EvalPatches(gpu::VertBuf *srcBuffer,
                                      BufferDescriptor const &srcDesc,
                                      gpu::VertBuf *dstBuffer,
                                      BufferDescriptor const &dstDesc,
                                      gpu::VertBuf *duBuffer,
                                      BufferDescriptor const &duDesc,
                                      gpu::VertBuf *dvBuffer,
                                      BufferDescriptor const &dvDesc,
                                      int numPatchCoords,
                                      gpu::VertBuf *patchCoordsBuffer,
                                      const PatchArrayVector &patchArrays,
                                      gpu::VertBuf *patchIndexBuffer,
                                      gpu::VertBuf *patchParamsBuffer)
{

  return EvalPatches(srcBuffer,
                     srcDesc,
                     dstBuffer,
                     dstDesc,
                     duBuffer,
                     duDesc,
                     dvBuffer,
                     dvDesc,
                     nullptr,
                     BufferDescriptor(),
                     nullptr,
                     BufferDescriptor(),
                     nullptr,
                     BufferDescriptor(),
                     numPatchCoords,
                     patchCoordsBuffer,
                     patchArrays,
                     patchIndexBuffer,
                     patchParamsBuffer);
}

bool GPUComputeEvaluator::EvalPatches(gpu::VertBuf *srcBuffer,
                                      BufferDescriptor const &srcDesc,
                                      gpu::VertBuf *dstBuffer,
                                      BufferDescriptor const &dstDesc,
                                      gpu::VertBuf *duBuffer,
                                      BufferDescriptor const &duDesc,
                                      gpu::VertBuf *dvBuffer,
                                      BufferDescriptor const &dvDesc,
                                      gpu::VertBuf *duuBuffer,
                                      BufferDescriptor const &duuDesc,
                                      gpu::VertBuf *duvBuffer,
                                      BufferDescriptor const &duvDesc,
                                      gpu::VertBuf *dvvBuffer,
                                      BufferDescriptor const &dvvDesc,
                                      int numPatchCoords,
                                      gpu::VertBuf *patchCoordsBuffer,
                                      const PatchArrayVector &patchArrays,
                                      gpu::VertBuf *patchIndexBuffer,
                                      gpu::VertBuf *patchParamsBuffer)
{

  if (_patchKernel.shader == nullptr) {
    return false;
  }

  GPU_shader_bind(_patchKernel.shader);
  GPU_vertbuf_bind_as_ssbo(srcBuffer, SHADER_SRC_VERTEX_BUFFER_BUF_SLOT);
  GPU_vertbuf_bind_as_ssbo(dstBuffer, SHADER_DST_VERTEX_BUFFER_BUF_SLOT);
  GPU_vertbuf_bind_as_ssbo(duBuffer, SHADER_DU_BUFFER_BUF_SLOT);
  GPU_vertbuf_bind_as_ssbo(dvBuffer, SHADER_DV_BUFFER_BUF_SLOT);
  if (duuBuffer) {
    GPU_vertbuf_bind_as_ssbo(duuBuffer, SHADER_DUU_BUFFER_BUF_SLOT);
  }
  if (duvBuffer) {
    GPU_vertbuf_bind_as_ssbo(duvBuffer, SHADER_DUV_BUFFER_BUF_SLOT);
  }
  if (dvvBuffer) {
    GPU_vertbuf_bind_as_ssbo(dvvBuffer, SHADER_DVV_BUFFER_BUF_SLOT);
  }
  GPU_vertbuf_bind_as_ssbo(patchCoordsBuffer, SHADER_PATCH_COORDS_BUF_SLOT);
  GPU_vertbuf_bind_as_ssbo(patchIndexBuffer, SHADER_PATCH_INDEX_BUFFER_BUF_SLOT);
  GPU_vertbuf_bind_as_ssbo(patchParamsBuffer, SHADER_PATCH_PARAM_BUFFER_BUF_SLOT);
  int patchArraySize = sizeof(PatchArray);
  if (_patchArraysSSBO) {
    GPU_storagebuf_free(_patchArraysSSBO);
    _patchArraysSSBO = nullptr;
  }
  _patchArraysSSBO = GPU_storagebuf_create_ex(patchArrays.size() * patchArraySize,
                                              static_cast<const void *>(&patchArrays[0]),
                                              GPU_USAGE_STATIC,
                                              "subdiv_patch_array");
  GPU_storagebuf_bind(_patchArraysSSBO, SHADER_PATCH_ARRAY_BUFFER_BUF_SLOT);

  GPU_shader_uniform_int_ex(
      _patchKernel.shader, _patchKernel.uniformSrcOffset, 1, 0, &srcDesc.offset);
  GPU_shader_uniform_int_ex(
      _patchKernel.shader, _patchKernel.uniformDstOffset, 1, 0, &dstDesc.offset);

// TODO init to -1 and check >= 0 to align with GPU module.
#define BIND_BUF_DESC(uniform, desc) \
  if (_stencilKernel.uniform > 0) { \
    int value[] = {desc.offset, desc.length, desc.stride}; \
    GPU_shader_uniform_int_ex(_patchKernel.shader, _stencilKernel.uniform, 3, 0, value); \
  }
  BIND_BUF_DESC(uniformDuDesc, duDesc)
  BIND_BUF_DESC(uniformDvDesc, dvDesc)
  BIND_BUF_DESC(uniformDuuDesc, duuDesc)
  BIND_BUF_DESC(uniformDuvDesc, duvDesc)
  BIND_BUF_DESC(uniformDvvDesc, dvvDesc)
#undef BIND_BUF_DESC

  DispatchCompute(_patchKernel.shader, numPatchCoords);
  GPU_shader_unbind();

  return true;
}
// ---------------------------------------------------------------------------

GPUComputeEvaluator::_StencilKernel::_StencilKernel() {}
GPUComputeEvaluator::_StencilKernel::~_StencilKernel()
{
  if (shader) {
    GPU_shader_free(shader);
    shader = nullptr;
  }
}

bool GPUComputeEvaluator::_StencilKernel::Compile(BufferDescriptor const &srcDesc,
                                                  BufferDescriptor const &dstDesc,
                                                  BufferDescriptor const &duDesc,
                                                  BufferDescriptor const &dvDesc,
                                                  BufferDescriptor const &duuDesc,
                                                  BufferDescriptor const &duvDesc,
                                                  BufferDescriptor const &dvvDesc,
                                                  int workGroupSize)
{
  // create stencil kernel
  if (shader) {
    GPU_shader_free(shader);
    shader = nullptr;
  }

  shader = compileKernel(
      srcDesc, dstDesc, duDesc, dvDesc, duuDesc, duvDesc, dvvDesc, true, workGroupSize);
  if (shader == nullptr) {
    return false;
  }

  // cache uniform locations (TODO: use uniform block)
  uniformStart = GPU_shader_get_uniform(shader, "batchStart");
  uniformEnd = GPU_shader_get_uniform(shader, "batchEnd");
  uniformSrcOffset = GPU_shader_get_uniform(shader, "srcOffset");
  uniformDstOffset = GPU_shader_get_uniform(shader, "dstOffset");
  uniformDuDesc = GPU_shader_get_uniform(shader, "duDesc");
  uniformDvDesc = GPU_shader_get_uniform(shader, "dvDesc");
  uniformDuuDesc = GPU_shader_get_uniform(shader, "duuDesc");
  uniformDuvDesc = GPU_shader_get_uniform(shader, "duvDesc");
  uniformDvvDesc = GPU_shader_get_uniform(shader, "dvvDesc");

  return true;
}

// ---------------------------------------------------------------------------

GPUComputeEvaluator::_PatchKernel::_PatchKernel() {}
GPUComputeEvaluator::_PatchKernel::~_PatchKernel()
{
  if (shader) {
    GPU_shader_free(shader);
    shader = nullptr;
  }
}

bool GPUComputeEvaluator::_PatchKernel::Compile(BufferDescriptor const &srcDesc,
                                                BufferDescriptor const &dstDesc,
                                                BufferDescriptor const &duDesc,
                                                BufferDescriptor const &dvDesc,
                                                BufferDescriptor const &duuDesc,
                                                BufferDescriptor const &duvDesc,
                                                BufferDescriptor const &dvvDesc,
                                                int workGroupSize)
{
  // create stencil kernel
  if (shader) {
    GPU_shader_free(shader);
    shader = nullptr;
  }

  shader = compileKernel(
      srcDesc, dstDesc, duDesc, dvDesc, duuDesc, duvDesc, dvvDesc, false, workGroupSize);
  if (shader == nullptr) {
    return false;
  }

  // cache uniform locations
  uniformSrcOffset = GPU_shader_get_uniform(shader, "srcOffset");
  uniformDstOffset = GPU_shader_get_uniform(shader, "dstOffset");
  uniformPatchArray = GPU_shader_get_uniform(shader, "patchArray");
  uniformDuDesc = GPU_shader_get_uniform(shader, "duDesc");
  uniformDvDesc = GPU_shader_get_uniform(shader, "dvDesc");
  uniformDuuDesc = GPU_shader_get_uniform(shader, "duuDesc");
  uniformDuvDesc = GPU_shader_get_uniform(shader, "duvDesc");
  uniformDvvDesc = GPU_shader_get_uniform(shader, "dvvDesc");

  return true;
}

}  // namespace blender::opensubdiv
