/* SPDX-FileCopyrightText: 2017 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#pragma once

#include "draw_curves_private.hh"
#include "draw_pass.hh"

namespace blender::bke {
class CurvesGeometry;
}

namespace blender::draw {

struct CurvesEvalCache;

class CurveRefinePass : public PassSimple {
 public:
  CurveRefinePass(const char *name) : PassSimple(name){};
};

using CurvesInfosBuf = UniformBuffer<CurvesInfos>;

struct CurvesUniformBufPool {
  Vector<std::unique_ptr<CurvesInfosBuf>> ubos;
  int used = 0;

  void reset()
  {
    used = 0;
    /* Allocate dummy. */
    alloc();
    ubos.first()->push_update();
  }

  CurvesInfosBuf &dummy_get()
  {
    return *ubos.first();
  }

  CurvesInfosBuf &alloc();
};

struct CurvesModule {
  CurvesUniformBufPool ubo_pool;
  CurveRefinePass refine = {"CurvesEvalPass"};
  /* Contains all transient input buffers contained inside `refine`.
   * Cleared after update. */
  Vector<gpu::VertBufPtr> transient_buffers;

  gpu::VertBuf *dummy_vbo = drw_curves_ensure_dummy_vbo();

  ~CurvesModule()
  {
    GPU_VERTBUF_DISCARD_SAFE(dummy_vbo);
  }

  void init()
  {
    ubo_pool.reset();

    refine.init();
    refine.state_set(DRW_STATE_NO_DRAW);
  }

  /* Record evaluation inside `refine`.
   * Output will be ready once `refine` pass has been submitted. */
  void evaluate_curve_attribute(const bke::CurvesGeometry &curve,
                                struct CurvesEvalCache &cache,
                                CurvesEvalShader shader_type,
                                gpu::VertBufPtr input_buf,
                                gpu::VertBufPtr &output_buf,
                                /* For radius during position evaluation. */
                                gpu::VertBuf *input2_buf = nullptr);

  void evaluate_positions(const bke::CurvesGeometry &curve,
                          struct CurvesEvalCache &cache,
                          gpu::VertBufPtr input_pos_buf,
                          gpu::VertBufPtr input_rad_buf,
                          gpu::VertBufPtr &output_pos_buf)
  {
    evaluate_curve_attribute(curve,
                             cache,
                             CURVES_EVAL_POSITION,
                             std::move(input_pos_buf),
                             output_pos_buf,
                             /* Transfer ownership through optional argument. */
                             input_rad_buf.release());
  }

  void evaluate_topology_indirection(const bke::CurvesGeometry &curve,
                                     struct CurvesEvalCache &cache,
                                     bool is_ribbon,
                                     gpu::VertBufPtr &output_indirection_buf);

 private:
  gpu::VertBuf *drw_curves_ensure_dummy_vbo();

  void dispatch(const bke::CurvesGeometry &curve, PassSimple::Sub &pass);
};

}  // namespace blender::draw

#define MAX_LAYER_NAME_CT 4 /* `u0123456789, u, au, a0123456789`. */
#define MAX_LAYER_NAME_LEN (GPU_MAX_SAFE_ATTR_NAME + 2)
#define MAX_THICKRES 2    /* see eHairType */
#define MAX_HAIR_SUBDIV 4 /* see hair_subdiv rna */

enum ParticleRefineShader {
  PART_REFINE_CATMULL_ROM = 0,
  PART_REFINE_MAX_SHADER,
};

struct ModifierData;
struct Object;
struct ParticleHairCache;
struct ParticleSystem;

struct ParticleHairFinalCache {
  /* Output of the subdivision stage: vertex buff sized to subdiv level. */
  blender::gpu::VertBuf *proc_buf;

  /* Just contains a huge index buffer used to draw the final hair. */
  blender::gpu::Batch *proc_hairs[MAX_THICKRES];

  int strands_res; /* points per hair, at least 2 */
};

struct ParticleHairCache {
  blender::gpu::VertBuf *pos;
  blender::gpu::IndexBuf *indices;
  blender::gpu::Batch *hairs;

  /* Hair Procedural display: Interpolation is done on the GPU. */
  blender::gpu::VertBuf *proc_point_buf; /* Input control points */

  /** Information of control points strands (segment count and base index) */
  blender::gpu::VertBuf *proc_strand_buf;

  /* Hair Length */
  blender::gpu::VertBuf *proc_length_buf;

  blender::gpu::VertBuf *proc_strand_seg_buf;

  blender::gpu::VertBuf *proc_uv_buf[MAX_MTFACE];
  blender::gpu::Texture *uv_tex[MAX_MTFACE];
  char uv_layer_names[MAX_MTFACE][MAX_LAYER_NAME_CT][MAX_LAYER_NAME_LEN];

  blender::gpu::VertBuf **proc_col_buf;
  blender::gpu::Texture **col_tex;
  char (*col_layer_names)[MAX_LAYER_NAME_CT][MAX_LAYER_NAME_LEN];

  int num_uv_layers;
  int num_col_layers;

  ParticleHairFinalCache final[MAX_HAIR_SUBDIV];

  int strands_len;
  int elems_len;
  int point_len;
};

namespace blender::draw {

/**
 * Ensure all textures and buffers needed for GPU accelerated drawing.
 */
bool particles_ensure_procedural_data(Object *object,
                                      ParticleSystem *psys,
                                      ModifierData *md,
                                      ParticleHairCache **r_hair_cache,
                                      GPUMaterial *gpu_material,
                                      int subdiv,
                                      int thickness_res);

}  // namespace blender::draw
