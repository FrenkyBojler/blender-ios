/* SPDX-FileCopyrightText: 2017 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#pragma once

#include <array>
#include <string>

#include "GPU_shader.hh"
#include "GPU_vertex_buffer.hh"

#include "BLI_vector_set.hh"

struct Curves;
namespace blender::bke {
class CurvesGeometry;
}  // namespace blender::bke

namespace blender::gpu {
class Batch;
class VertBuf;
}  // namespace blender::gpu
struct GPUMaterial;

namespace blender::draw {

struct CurvesModule;

#define MAX_FACE_PER_SEGMENT 3 /* see eHairType */
#define MAX_HAIR_SUBDIV 4      /* see hair_subdiv rna */

enum CurvesEvalShader {
  CURVES_EVAL_POSITION = 0,
  CURVES_EVAL_FLOAT = 1,
  CURVES_EVAL_FLOAT2 = 2,
  CURVES_EVAL_FLOAT3 = 3,
  CURVES_EVAL_FLOAT4 = 4,
};

#define CURVES_EVAL_SHADER_NUM 5

/* Curves procedural display: Evaluation is done on the GPU. */
struct CurvesEvalCache {
  /* --- Required attributes. --- */

  /** Position and radius per evaluated point. Always evaluated. */
  gpu::VertBufPtr evaluated_pos_rad_buf;

  /** Intercept time per evaluated point. */
  /* TODO(fclem): Move it to generic point domain attributes. */
  gpu::VertBufPtr evaluated_time_buf;
  /** Intercept time per curve. */
  /* TODO(fclem): Move it to generic curve domain attributes. */
  gpu::VertBufPtr curves_length_buf;

  /* --- Indirection buffers. --- */

  /* Map primitive to point ID and curve ID. Contains restart indices for line and triangle strip
   * primitive. */
  gpu::VertBufPtr indirection_ribbon_buf;
  /* Map primitive to point ID and curve ID. Compacted for cylinder primitive. */
  gpu::VertBufPtr indirection_cylinder_buf;

  /* --- Buffers common to all curve types. --- */

  /** Buffer containing `CurveGeometry::points_by_curve()`. */
  gpu::VertBufPtr points_by_curve_buf;
  /** Buffer containing `CurveGeometry::evaluated_points_by_curve()`. */
  gpu::VertBufPtr evaluated_points_by_curve_buf;
  /** Buffer containing `CurveGeometry::curve_types()`. */
  gpu::VertBufPtr curves_type_buf;
  /** Buffer containing `CurveGeometry::resolution()`. */
  gpu::VertBufPtr curves_resolution_buf;

  /* --- Buffers only needed if geometry has Bezier curves. Dummy sized otherwise. --- */

  /** Buffer containing `CurveGeometry::handle_positions_left()`. */
  gpu::VertBufPtr handles_positions_left_buf;
  /** Buffer containing `CurveGeometry::handle_positions_right()`. */
  gpu::VertBufPtr handles_positions_right_buf;
  /** Buffer containing `EvaluatedOffsets::all_bezier_offsets`. */
  gpu::VertBufPtr bezier_offsets_buf;

  /* --- Buffers only needed if geometry has Nurbs curves. Dummy sized otherwise. --- */

  /** Buffer containing `CurveGeometry::nurbs_orders()`. */
  gpu::VertBufPtr curves_order_buf;
  /** Buffer containing `CurveGeometry::nurbs_weights()`. */
  gpu::VertBufPtr control_weights_buf;
  /** Buffer containing all `nurbs::BasisCache` concatenated. */
  gpu::VertBufPtr basis_cache_buf;
  /** Buffer containing offsets to the start of each `nurbs::BasisCache` for each curve. */
  gpu::VertBufPtr basis_cache_offset_buf;

  /* --- Generic Attributes. --- */

  /** Attributes currently being drawn or about to be drawn. */
  VectorSet<std::string> attr_used;
  /**
   * Attributes that were used at some point. This is used for garbage collection, to remove
   * attributes that are not used in shaders anymore due to user edits.
   */
  VectorSet<std::string> attr_used_over_time;
  /**
   * The last time in seconds that the `attr_used` and `attr_used_over_time` were exactly the same.
   * If the delta between this time and the current scene time is greater than the timeout set in
   * user preferences (`U.vbotimeout`) then garbage collection is performed.
   */
  int last_attr_matching_time;
  /* Attributes stored per curve. Nullptr if attribute is not from this domain. */
  gpu::VertBufPtr curve_attributes_buf[GPU_MAX_ATTR];
  /* Output of the evaluation stage. This is only used by attributes on point domain. */
  gpu::VertBufPtr evaluated_attributes_buf[GPU_MAX_ATTR];
  /* If attribute is point domain, use evaluated_attributes_buf. Otherwise curve_attributes_buf. */
  std::array<bool, GPU_MAX_ATTR> attributes_point_domain;

  /* --- Procedural Drawcalls. --- */
  std::array<gpu::Batch *, MAX_FACE_PER_SEGMENT> batch;

  void ensure_attribute(struct CurvesModule &module,
                        const bke::CurvesGeometry &curves,
                        const StringRef name,
                        int index);
  void ensure_attributes(struct CurvesModule &module,
                         const bke::CurvesGeometry &curves,
                         const GPUMaterial *gpu_material);

  void ensure_common(const bke::CurvesGeometry &curves);
  void ensure_bezier(const bke::CurvesGeometry &curves);
  void ensure_nurbs(const bke::CurvesGeometry &curves);

  void ensure_positions(CurvesModule &module, const bke::CurvesGeometry &curves);

  gpu::VertBufPtr &indirection_buf_get(CurvesModule &module,
                                       const bke::CurvesGeometry &curves,
                                       int face_per_segment);

  gpu::Batch *batch_get(const bke::CurvesGeometry &curves, int face_per_segment);
};

CurvesEvalCache &curves_get_eval_cache(Curves &curves_id);

void drw_curves_get_attribute_sampler_name(StringRef layer_name, char r_sampler_name[32]);

}  // namespace blender::draw
