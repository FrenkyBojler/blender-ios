/* SPDX-FileCopyrightText: 2016 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#include "DNA_material_types.h"
#include "DNA_world_types.h"

#include "DRW_render.hh"

GPUMaterial *DRW_shader_from_world(World *wo,
                                   bNodeTree *ntree,
                                   eGPUMaterialEngine engine,
                                   const uint64_t shader_id,
                                   bool deferred,
                                   GPUCodegenCallbackFn callback,
                                   void *thunk)
{
  return GPU_material_from_nodetree(
      nullptr, ntree, &wo->gpumaterial, wo->id.name, engine, shader_id, deferred, callback, thunk);
}

GPUMaterial *DRW_shader_from_material(Material *ma,
                                      bNodeTree *ntree,
                                      eGPUMaterialEngine engine,
                                      const uint64_t shader_id,
                                      bool deferred,
                                      GPUCodegenCallbackFn callback,
                                      void *thunk,
                                      GPUMaterialPassReplacementCallbackFn pass_replacement_cb)
{
  return GPU_material_from_nodetree(ma,
                                    ntree,
                                    &ma->gpumaterial,
                                    ma->id.name,
                                    engine,
                                    shader_id,
                                    deferred,
                                    callback,
                                    thunk,
                                    pass_replacement_cb);
}
