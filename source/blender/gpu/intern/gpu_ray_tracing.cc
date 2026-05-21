#include "GPU_ray_tracing.hh"
#include "GPU_capabilities.hh"

#include "gpu_backend.hh"

#include "BLI_assert.h"

using namespace blender::gpu;

TopLevelAS *GPU_ray_tracing_tlas_alloc(const char *name)
{
  BLI_assert(GPU_ray_query_support());
  return GPUBackend::get()->tlas_alloc(name);
}

BottomLevelAS *GPU_ray_tracing_blas_alloc(const char *name)
{
  BLI_assert(GPU_ray_query_support());
  return GPUBackend::get()->blas_alloc(name);
}

void GPU_ray_tracing_tlas_discard(TopLevelAS *tlas)
{
  delete tlas;
}

void GPU_ray_tracing_blas_discard(BottomLevelAS *blas)
{
  delete blas;
}
