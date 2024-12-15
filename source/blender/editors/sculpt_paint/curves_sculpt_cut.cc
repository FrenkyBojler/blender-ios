/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_crazyspace.hh"
#include "BKE_paint.hh"

#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "DEG_depsgraph.hh"

#include "DNA_brush_types.h"

#include "WM_api.hh"

#include "BLI_array_utils.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_noise.hh"
#include "BLI_task.hh"

#include "GEO_trim_curves.hh"

#include "curves_sculpt_intern.hh"

namespace blender::ed::sculpt_paint {

class CutOperation : public CurvesSculptStrokeOperation {
 private:
  /** Only used when a 3D brush is used. */
  CurvesBrush3D brush_3d_;

  friend struct CutOperationExecutor;

 public:
  void on_stroke_extended(const bContext &C, const StrokeExtension &stroke_extension) override;
};

struct BrushProjectionInfo {
  float distance = std::numeric_limits<float>::max();
  union {
    /** Used for projection falloff. */
    float4x4 brush_transform_inv;
    /** Used for sphere falloff. */
    float3 brush_pos_cu;
  };
};

/**
 * Utility class that actually executes the update when the stroke is updated. That's useful
 * because it avoids passing a very large number of parameters between functions.
 */
struct CutOperationExecutor {
  CutOperation *self_ = nullptr;
  CurvesSculptCommonContext ctx_;

  Object *object_ = nullptr;
  Curves *curves_id_ = nullptr;
  CurvesGeometry *curves_ = nullptr;

  VArray<float> point_factors_;
  IndexMaskMemory selected_curve_memory_;
  IndexMask curve_selection_;

  const CurvesSculpt *curves_sculpt_ = nullptr;
  const Brush *brush_ = nullptr;
  float brush_radius_base_re_;
  float brush_radius_factor_;
  float2 brush_pos_re_;
  float brush_strength_;

  CurvesSurfaceTransforms transforms_;

  CutOperationExecutor(const bContext &C) : ctx_(C) {}

  void execute(CutOperation &self, const bContext &C, const StrokeExtension &stroke_extension)
  {
    UNUSED_VARS(C, stroke_extension);
    self_ = &self;

    object_ = CTX_data_active_object(&C);
    curves_id_ = static_cast<Curves *>(object_->data);
    curves_ = &curves_id_->geometry.wrap();
    if (curves_->is_empty()) {
      return;
    }

    curve_selection_ = curves::retrieve_selected_curves(*curves_id_, selected_curve_memory_);
    if (curve_selection_.is_empty()) {
      return;
    }

    curves_sculpt_ = ctx_.scene->toolsettings->curves_sculpt;
    brush_ = BKE_paint_brush_for_read(&curves_sculpt_->paint);
    brush_radius_base_re_ = BKE_brush_size_get(ctx_.scene, brush_);
    brush_radius_factor_ = brush_radius_factor(*brush_, stroke_extension);
    brush_pos_re_ = stroke_extension.mouse_position;
    brush_strength_ = brush_strength_get(*ctx_.scene, *brush_, stroke_extension);

    point_factors_ = *curves_->attributes().lookup_or_default<float>(
        ".selection", bke::AttrDomain::Point, 1.0f);
    transforms_ = CurvesSurfaceTransforms(*object_, curves_id_->surface);

    const eBrushFalloffShape falloff_shape = eBrushFalloffShape(brush_->falloff_shape);
    if (stroke_extension.is_first) {
      if (falloff_shape == PAINT_FALLOFF_SHAPE_SPHERE) {
        self.brush_3d_ = *sample_curves_3d_brush(*ctx_.depsgraph,
                                                 *ctx_.region,
                                                 *ctx_.v3d,
                                                 *ctx_.rv3d,
                                                 *object_,
                                                 brush_pos_re_,
                                                 brush_radius_base_re_);
      }
    }

    Array<BrushProjectionInfo> brush_projection_info(curves_->points_num());
    bool includes_cyclic = false;
    if (falloff_shape == PAINT_FALLOFF_SHAPE_TUBE) {
      this->find_projected_points_in_stroke_with_symmetry(includes_cyclic, brush_projection_info);
    }
    else if (falloff_shape == PAINT_FALLOFF_SHAPE_SPHERE) {
      this->find_spherical_points_in_stroke_with_symmetry(includes_cyclic, brush_projection_info);
    }
    else {
      BLI_assert_unreachable();
    }

    Array<bool> curves_to_keep(curves_->curves_num(), true);
    Array<float> ends(curves_->curves_num(), std::numeric_limits<float>::max());

    this->cut_curves(brush_projection_info, curves_to_keep, ends);

    IndexMaskMemory mask_memory;
    const IndexMask mask_to_keep = IndexMask::from_bools(curves_to_keep, mask_memory);

    *curves_ = bke::curves_copy_curve_selection(*curves_, mask_to_keep, {});
    curves_->tag_topology_changed();

    Array<float> kept_ends(mask_to_keep.size());
    array_utils::gather(ends.as_span(), mask_to_keep, kept_ends.as_mutable_span());

    if (!curves_->is_empty()) {
      *curves_ = geometry::trim_curves(*curves_,
                                       IndexMask(curves_->curves_num()),
                                       VArray<float>::ForSingle(0.0f, curves_->curves_num()),
                                       VArray<float>::ForSpan(kept_ends),
                                       GeometryNodeCurveSampleMode::GEO_NODE_CURVE_SAMPLE_LENGTH,
                                       {});
    }

    if (includes_cyclic) {
      report_cyclic_not_supported(stroke_extension.reports);
    }

    DEG_id_tag_update(&curves_id_->id, ID_RECALC_GEOMETRY);
    WM_main_add_notifier(NC_GEOM | ND_DATA, &curves_id_->id);
    ED_region_tag_redraw(ctx_.region);
  }

  void find_projected_points_in_stroke_with_symmetry(
      bool &r_includes_cyclic, MutableSpan<BrushProjectionInfo> r_brush_projection_info)
  {
    const Vector<float4x4> symmetry_brush_transforms = get_symmetry_brush_transforms(
        eCurvesSymmetryType(curves_id_->symmetry));
    for (const float4x4 &brush_transform : symmetry_brush_transforms) {
      this->find_projected_points_in_stroke(
          brush_transform, r_includes_cyclic, r_brush_projection_info);
    }
  }

  void find_projected_points_in_stroke(const float4x4 &brush_transform,
                                       bool &r_includes_cyclic,
                                       MutableSpan<BrushProjectionInfo> r_brush_projection_info)
  {
    const float4x4 brush_transform_inv = math::invert(brush_transform);

    const float4x4 projection = ED_view3d_ob_project_mat_get(ctx_.rv3d, object_);

    const bke::crazyspace::GeometryDeformation deformation =
        bke::crazyspace::get_evaluated_curves_deformation(*ctx_.depsgraph, *object_);
    const OffsetIndices points_by_curve = curves_->points_by_curve();

    const VArray<bool> cyclic = curves_->cyclic();

    curve_selection_.foreach_index(GrainSize(256), [&](const int curve_i) {
      if (cyclic[curve_i]) {
        r_includes_cyclic = true;
        return;
      }

      const IndexRange points = points_by_curve[curve_i];
      for (const int i : IndexRange(points.size())) {
        const int point_i = points[i];

        const float3 &pos_cu = math::transform_point(brush_transform_inv,
                                                     deformation.positions[point_i]);
        const float2 pos_re = ED_view3d_project_float_v2_m4(ctx_.region, pos_cu, projection);
        const float dist_to_brush_sq_re = math::distance_squared(pos_re, brush_pos_re_);
        if (dist_to_brush_sq_re < r_brush_projection_info[point_i].distance) {
          r_brush_projection_info[point_i].distance = dist_to_brush_sq_re;
          r_brush_projection_info[point_i].brush_transform_inv = brush_transform_inv;
        }
      }
    });
  }

  void find_spherical_points_in_stroke_with_symmetry(
      bool &r_includes_cyclic, MutableSpan<BrushProjectionInfo> r_brush_projection_info)
  {
    float3 brush_pos_wo;
    ED_view3d_win_to_3d(
        ctx_.v3d,
        ctx_.region,
        math::transform_point(transforms_.curves_to_world, self_->brush_3d_.position_cu),
        brush_pos_re_,
        brush_pos_wo);
    const float3 brush_pos_cu = math::transform_point(transforms_.world_to_curves, brush_pos_wo);

    const Vector<float4x4> symmetry_brush_transforms = get_symmetry_brush_transforms(
        eCurvesSymmetryType(curves_id_->symmetry));
    for (const float4x4 &brush_transform : symmetry_brush_transforms) {
      this->find_spherical_points_in_stroke(math::transform_point(brush_transform, brush_pos_cu),
                                            r_includes_cyclic,
                                            r_brush_projection_info);
    }
  }

  void find_spherical_points_in_stroke(const float3 &brush_pos_cu,
                                       bool &r_includes_cyclic,
                                       MutableSpan<BrushProjectionInfo> r_brush_projection_info)
  {
    const bke::crazyspace::GeometryDeformation deformation =
        bke::crazyspace::get_evaluated_curves_deformation(*ctx_.depsgraph, *object_);
    const OffsetIndices points_by_curve = curves_->points_by_curve();

    const VArray<bool> cyclic = curves_->cyclic();

    curve_selection_.foreach_index(GrainSize(256), [&](const int curve_i) {
      if (cyclic[curve_i]) {
        r_includes_cyclic = true;
        return;
      }

      const IndexRange points = points_by_curve[curve_i];
      for (const int i : IndexRange(points.size())) {
        const int point_i = points[i];

        const float3 &pos_cu = deformation.positions[point_i];
        const float dist_to_brush_sq_cu = math::distance_squared(pos_cu, brush_pos_cu);
        if (dist_to_brush_sq_cu < r_brush_projection_info[point_i].distance) {
          r_brush_projection_info[point_i].distance = dist_to_brush_sq_cu;
          r_brush_projection_info[point_i].brush_pos_cu = brush_pos_cu;
        }
      }
    });
  }

  /**
   * \param stroke_hash: A hash which should try to return different results for each iteration of
   * the cut. Used for "blunt scissors" mode, based on brush strength.
   */
  bool should_point_be_cut(const int point_i, const uint32_t stroke_hash)
  {
    if (point_factors_[point_i] <= 0.0f) {
      return false;
    }

    if (brush_strength_ < 1.0f) {
      const float stroke_chance = noise::hash_to_float(stroke_hash);
      if (stroke_chance > brush_strength_) {
        return false;
      }
    }

    return true;
  }

  float3 find_projected_cut_boundary(const float3 &point_outside_cu,
                                     const float3 &point_inside_cu,
                                     const float2 &brush_pos_re,
                                     const float brush_radius_re,
                                     const float4x4 &transform_cu_to_re)
  {
    const float2 point_outside_re = ED_view3d_project_float_v2_m4(
        ctx_.region, point_outside_cu, transform_cu_to_re);
    const float2 point_inside_re = ED_view3d_project_float_v2_m4(
        ctx_.region, point_inside_cu, transform_cu_to_re);

    const float2 line_re = point_inside_re - point_outside_re;
    const float2 brush_to_outside_re = point_outside_re - brush_pos_re;

    const float a = dot_v2v2(line_re, line_re);
    const float b = 2.0f * dot_v2v2(line_re, brush_to_outside_re);
    const float c = dot_v2v2(brush_to_outside_re, brush_to_outside_re) -
                    brush_radius_re * brush_radius_re;

    const float d = b * b - 4.0f * a * c;
    const float t = (-b - sqrtf(d)) / (2.0f * a);

    const float3 line_cu = point_inside_cu - point_outside_cu;
    const float3 intersection_cu = point_outside_cu + t * line_cu;
    return intersection_cu;
  }

  float3 find_spherical_cut_boundary(const float3 &point_outside_cu,
                                     const float3 &point_inside_cu,
                                     const float3 &brush_pos_cu,
                                     const float brush_radius_cu)
  {
    const float3 line_cu = point_inside_cu - point_outside_cu;
    const float3 brush_to_outside_cu = point_outside_cu - brush_pos_cu;

    const float a = dot_v3v3(line_cu, line_cu);
    const float b = 2.0f * dot_v3v3(line_cu, brush_to_outside_cu);
    const float c = dot_v3v3(brush_to_outside_cu, brush_to_outside_cu) -
                    brush_radius_cu * brush_radius_cu;

    const float d = b * b - 4.0f * a * c;
    const float t = (-b - sqrtf(d)) / (2.0f * a);

    const float3 intersection_cu = point_outside_cu + t * line_cu;
    return intersection_cu;
  }

  void cut_curves(const Span<BrushProjectionInfo> brush_projection_info,
                  MutableSpan<bool> r_curves_to_keep,
                  MutableSpan<float> r_ends)
  {
    const OffsetIndices points_by_curve = curves_->points_by_curve();
    const Array<float> point_lengths = calculate_point_lengths();

    const bke::crazyspace::GeometryDeformation deformation =
        bke::crazyspace::get_evaluated_curves_deformation(*ctx_.depsgraph, *object_);
    const float4x4 projection = ED_view3d_ob_project_mat_get(ctx_.rv3d, object_);

    /** cu or re, depending on falloff shape */
    float brush_radius;

    const eBrushFalloffShape falloff_shape = eBrushFalloffShape(brush_->falloff_shape);
    if (falloff_shape == PAINT_FALLOFF_SHAPE_TUBE) {
      brush_radius = brush_radius_base_re_ * brush_radius_factor_;
    }
    else if (falloff_shape == PAINT_FALLOFF_SHAPE_SPHERE) {
      brush_radius = self_->brush_3d_.radius_cu * brush_radius_factor_;
    }
    else {
      BLI_assert_unreachable();
    }

    const float brush_radius_sq = pow2f(brush_radius);
    const uint64_t brush_pos_hash = brush_pos_re_.hash();

    curve_selection_.foreach_index(GrainSize(256), [&](const int curve_i) {
      const IndexRange points = points_by_curve[curve_i];
      const Span<BrushProjectionInfo> brush_projection_info_slice = brush_projection_info.slice(
          points);

      const BrushProjectionInfo *first_point_in_stroke_bpi = std::find_if(
          brush_projection_info_slice.begin(),
          brush_projection_info_slice.end(),
          [&](const BrushProjectionInfo projection_info) {
            return projection_info.distance <= brush_radius_sq;
          });
      if (first_point_in_stroke_bpi == brush_projection_info_slice.end()) {
        return;
      }
      const int first_point_in_stroke = std::distance(brush_projection_info_slice.begin(),
                                                      first_point_in_stroke_bpi);

      const IndexRange::Iterator point_to_cut_iter = std::find_if(
          points.begin(), points.end(), [&](const int point_i) {
            const BrushProjectionInfo &bpi = brush_projection_info[point_i];
            if (bpi.distance > brush_radius_sq) {
              return false;
            }

            const uint32_t point_hash = noise::hash(
                noise::hash_float(first_point_in_stroke_bpi->distance), brush_pos_hash);
            return should_point_be_cut(point_i, point_hash);
          });
      if (point_to_cut_iter == points.end()) {
        return;
      }
      const int point_to_cut = std::distance(points.begin(), point_to_cut_iter);

      if (point_to_cut == 0) {
        // Delete entire curve. Simply trimming would leave behind the root control point.
        r_curves_to_keep[curve_i] = false;
      }
      else if (first_point_in_stroke == point_to_cut) {
        // Brush boundary is cutting straight through previous and current point. Delete all points
        // after current.
        const int current_point = points[point_to_cut];
        const int previous_point = points[point_to_cut - 1];
        const float3 &curr_pos_cu = deformation.positions[current_point];
        const float3 &prev_pos_cu = deformation.positions[previous_point];
        float3 boundary_cu;

        if (falloff_shape == PAINT_FALLOFF_SHAPE_TUBE) {
          boundary_cu = math::transform_point(
              first_point_in_stroke_bpi->brush_transform_inv,
              find_projected_cut_boundary(
                  prev_pos_cu, curr_pos_cu, brush_pos_re_, brush_radius, projection));
        }
        else if (falloff_shape == PAINT_FALLOFF_SHAPE_SPHERE) {
          boundary_cu = find_spherical_cut_boundary(
              prev_pos_cu, curr_pos_cu, first_point_in_stroke_bpi->brush_pos_cu, brush_radius);
        }
        else {
          BLI_assert_unreachable();
        }

        // TODO: Use proper evaluation function for point on curve (based on curve type).
        const float boundary_length = math::distance(curr_pos_cu, boundary_cu);
        r_ends[curve_i] = point_lengths[current_point] - boundary_length;
      }
      else {
        // Brush is encompassing a boundary between selected and unselected points.
        const int previous_point = points[point_to_cut - 1];
        r_ends[curve_i] = point_lengths[previous_point];
      }
    });
  }

  /**
   * TODO: COPIED from curve_spline_parameter.cc
   * Return the length of each control point along each curve, starting at zero for the first
   * point. Importantly, this is different than the length at each evaluated point. The
   * implementation is different for every curve type:
   *  - Catmull Rom Curves: Use the resolution to find the evaluated point for each control point.
   *  - Poly Curves: Copy the evaluated lengths, but we need to add a zero to the front of the
   * array.
   *  - Bezier Curves: Use the evaluated offsets to find the evaluated point for each control
   * point.
   *  - NURBS Curves: Treat the control points as if they were a poly curve, because there
   *    is no obvious mapping from each control point to a specific evaluated point.
   */
  Array<float> calculate_point_lengths()
  {
    curves_->ensure_evaluated_lengths();
    const OffsetIndices points_by_curve = curves_->points_by_curve();
    const VArray<int8_t> types = curves_->curve_types();
    const VArray<int> resolutions = curves_->resolution();
    const VArray<bool> cyclic = curves_->cyclic();

    Array<float> result(curves_->points_num());

    threading::parallel_for(curves_->curves_range(), 128, [&](IndexRange range) {
      for (const int i_curve : range) {
        const IndexRange points = points_by_curve[i_curve];
        const bool is_cyclic = cyclic[i_curve];
        const Span<float> evaluated_lengths = curves_->evaluated_lengths_for_curve(i_curve,
                                                                                   is_cyclic);
        MutableSpan<float> lengths = result.as_mutable_span().slice(points);
        lengths.first() = 0.0f;
        const float last_evaluated_length = evaluated_lengths.is_empty() ?
                                                0.0f :
                                                evaluated_lengths.last();

        float total;
        switch (types[i_curve]) {
          case CURVE_TYPE_CATMULL_ROM: {
            const int resolution = resolutions[i_curve];
            for (const int i : IndexRange(points.size()).drop_back(1)) {
              lengths[i + 1] = evaluated_lengths[resolution * (i + 1) - 1];
            }
            total = last_evaluated_length;
            break;
          }
          case CURVE_TYPE_POLY:
            lengths.drop_front(1).copy_from(evaluated_lengths.take_front(lengths.size() - 1));
            total = last_evaluated_length;
            break;
          case CURVE_TYPE_BEZIER: {
            const Span<int> offsets = curves_->bezier_evaluated_offsets_for_curve(i_curve);
            for (const int i : IndexRange(points.size()).drop_back(1)) {
              lengths[i + 1] = evaluated_lengths[offsets[i + 1] - 1];
            }
            total = last_evaluated_length;
            break;
          }
          case CURVE_TYPE_NURBS: {
            const Span<float3> positions = curves_->positions().slice(points);
            float length = 0.0f;
            for (const int i : positions.index_range().drop_back(1)) {
              lengths[i] = length;
              length += math::distance(positions[i], positions[i + 1]);
            }
            lengths.last() = length;
            if (is_cyclic) {
              length += math::distance(positions.first(), positions.last());
            }
            total = length;
            break;
          }
        }
      }
    });
    return result;
  }
};

void CutOperation::on_stroke_extended(const bContext &C, const StrokeExtension &stroke_extension)
{
  CutOperationExecutor executor{C};
  executor.execute(*this, C, stroke_extension);
}

std::unique_ptr<CurvesSculptStrokeOperation> new_cut_operation()
{
  return std::make_unique<CutOperation>();
}

}  // namespace blender::ed::sculpt_paint
