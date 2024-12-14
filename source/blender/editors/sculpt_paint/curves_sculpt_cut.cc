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

#include "BLI_enumerable_thread_specific.hh"
#include "BLI_ghash.h"
#include "BLI_rand.h"
#include "BLI_task.hh"

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

    curves_sculpt_ = ctx_.scene->toolsettings->curves_sculpt;
    brush_ = BKE_paint_brush_for_read(&curves_sculpt_->paint);
    brush_radius_base_re_ = BKE_brush_size_get(ctx_.scene, brush_);
    brush_radius_factor_ = brush_radius_factor(*brush_, stroke_extension);
    brush_pos_re_ = stroke_extension.mouse_position;
    brush_strength_ = brush_strength_get(*ctx_.scene, *brush_, stroke_extension);

    point_factors_ = *curves_->attributes().lookup_or_default<float>(
        ".selection", bke::AttrDomain::Point, 1.0f);
    curve_selection_ = curves::retrieve_selected_curves(*curves_id_, selected_curve_memory_);
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

    /** First point's index on curve that gets deleted, or -1 to skip. */
    Array<int> curves_cut_points(curves_->curves_num(), -1);
    /** If curves_cut_points > 0, this decides the new position of the previous point. */
    Array<float3> curves_cut_positions(curves_->curves_num());

    if (falloff_shape == PAINT_FALLOFF_SHAPE_TUBE) {
      this->find_projected_points_in_stroke_with_symmetry(curves_cut_points, curves_cut_positions);
    }
    else if (falloff_shape == PAINT_FALLOFF_SHAPE_SPHERE) {
      this->find_spherical_points_in_stroke_with_symmetry(curves_cut_points, curves_cut_positions);
    }
    else {
      BLI_assert_unreachable();
    }

    bool includes_cyclic = false;
    this->cut(curves_cut_points, curves_cut_positions, includes_cyclic);
    if (includes_cyclic) {
      report_cyclic_not_supported(stroke_extension.reports);
    }

    curves_->tag_topology_changed();
    DEG_id_tag_update(&curves_id_->id, ID_RECALC_GEOMETRY);
    WM_main_add_notifier(NC_GEOM | ND_DATA, &curves_id_->id);
    ED_region_tag_redraw(ctx_.region);
  }

  void find_projected_points_in_stroke_with_symmetry(MutableSpan<int> r_curves_cut_points,
                                                     MutableSpan<float3> r_curves_cut_positions)
  {
    const Vector<float4x4> symmetry_brush_transforms = get_symmetry_brush_transforms(
        eCurvesSymmetryType(curves_id_->symmetry));
    for (const float4x4 &brush_transform : symmetry_brush_transforms) {
      this->find_projected_points_in_stroke(
          brush_transform, r_curves_cut_points, r_curves_cut_positions);
    }
  }

  void find_projected_points_in_stroke(const float4x4 &brush_transform,
                                       MutableSpan<int> r_curves_cut_points,
                                       MutableSpan<float3> r_curves_cut_positions)
  {
    const float4x4 brush_transform_inv = math::invert(brush_transform);

    const float brush_radius_re = brush_radius_base_re_ * brush_radius_factor_;
    const float brush_radius_sq_re = pow2f(brush_radius_re);
    const uint64_t brush_pos_hash = brush_pos_re_.hash();

    const float4x4 projection = ED_view3d_ob_project_mat_get(ctx_.rv3d, object_);

    const bke::crazyspace::GeometryDeformation deformation =
        bke::crazyspace::get_evaluated_curves_deformation(*ctx_.depsgraph, *object_);
    const OffsetIndices points_by_curve = curves_->points_by_curve();

    curve_selection_.foreach_index(GrainSize(256), [&](const int curve_i) {
      const IndexRange points = points_by_curve[curve_i];
      int first_point_in_stroke = -1;
      for (const int i : IndexRange(points.size())) {
        const int point_i = points[i];
        const float3 &pos_cu = math::transform_point(brush_transform_inv,
                                                     deformation.positions[point_i]);
        const float2 pos_re = ED_view3d_project_float_v2_m4(ctx_.region, pos_cu, projection);
        const float dist_to_brush_sq_re = math::distance_squared(pos_re, brush_pos_re_);
        if (dist_to_brush_sq_re > brush_radius_sq_re) {
          continue;
        }

        if (first_point_in_stroke < 0) {
          first_point_in_stroke = i;
        }

        if (!should_point_be_cut(point_i,
                                 BLI_ghashutil_combine_hash(pos_cu.hash(), brush_pos_hash)))
        {
          continue;
        }

        if (i == 0) {
          // Delete root.
          r_curves_cut_points[curve_i] = 0;
          r_curves_cut_positions[curve_i] = deformation.positions[point_i];
        }
        else if (first_point_in_stroke == i) {
          // Brush boundary is cutting straight through i-1 and i. Delete all points after i.
          const int prev_point_i = points[i - 1];
          r_curves_cut_points[curve_i] = i + 1;
          r_curves_cut_positions[curve_i] = find_projected_cut_boundary(
              deformation.positions[prev_point_i],
              deformation.positions[point_i],
              brush_pos_re_,
              brush_radius_re,
              projection);
        }
        else {
          // Brush is encompassing a boundary between selected and unselected points.
          // Previous point's position should be untouched.
          const int prev_point_i = points[i - 1];
          r_curves_cut_points[curve_i] = i;
          r_curves_cut_positions[curve_i] = deformation.positions[prev_point_i];
        }

        break;
      }
    });
  }

  void find_spherical_points_in_stroke_with_symmetry(MutableSpan<int> r_curves_cut_points,
                                                     MutableSpan<float3> r_curves_cut_positions)
  {
    float3 brush_pos_wo;
    ED_view3d_win_to_3d(
        ctx_.v3d,
        ctx_.region,
        math::transform_point(transforms_.curves_to_world, self_->brush_3d_.position_cu),
        brush_pos_re_,
        brush_pos_wo);
    const float3 brush_pos_cu = math::transform_point(transforms_.world_to_curves, brush_pos_wo);
    const float brush_radius_cu = self_->brush_3d_.radius_cu * brush_radius_factor_;

    const Vector<float4x4> symmetry_brush_transforms = get_symmetry_brush_transforms(
        eCurvesSymmetryType(curves_id_->symmetry));
    for (const float4x4 &brush_transform : symmetry_brush_transforms) {
      this->find_spherical_points_in_stroke(math::transform_point(brush_transform, brush_pos_cu),
                                            brush_radius_cu,
                                            r_curves_cut_points,
                                            r_curves_cut_positions);
    }
  }

  void find_spherical_points_in_stroke(const float3 &brush_pos_cu,
                                       const float brush_radius_cu,
                                       MutableSpan<int> r_curves_cut_points,
                                       MutableSpan<float3> r_curves_cut_positions)
  {
    const float brush_radius_sq_cu = pow2f(brush_radius_cu);
    const uint64_t brush_pos_hash = brush_pos_cu.hash();
    const bke::crazyspace::GeometryDeformation deformation =
        bke::crazyspace::get_evaluated_curves_deformation(*ctx_.depsgraph, *object_);
    const OffsetIndices points_by_curve = curves_->points_by_curve();

    curve_selection_.foreach_index(GrainSize(256), [&](const int curve_i) {
      const IndexRange points = points_by_curve[curve_i];
      int first_point_in_stroke = -1;
      for (const int i : IndexRange(points.size())) {
        const int point_i = points[i];
        const float3 &pos_cu = deformation.positions[point_i];
        const float dist_to_brush_sq_cu = math::distance_squared(pos_cu, brush_pos_cu);
        if (dist_to_brush_sq_cu > brush_radius_sq_cu) {
          continue;
        }

        if (first_point_in_stroke < 0) {
          first_point_in_stroke = i;
        }

        if (!should_point_be_cut(point_i,
                                 BLI_ghashutil_combine_hash(pos_cu.hash(), brush_pos_hash)))
        {
          continue;
        }

        if (i == 0) {
          // Delete root.
          r_curves_cut_points[curve_i] = 0;
          r_curves_cut_positions[curve_i] = deformation.positions[point_i];
        }
        else if (first_point_in_stroke == i) {
          // Brush boundary is cutting straight through i-1 and i. Delete all points after i.
          const int prev_point_i = points[i - 1];
          r_curves_cut_points[curve_i] = i + 1;
          r_curves_cut_positions[curve_i] = find_spherical_cut_boundary(
              deformation.positions[prev_point_i],
              deformation.positions[point_i],
              brush_pos_cu,
              brush_radius_cu);
        }
        else {
          // Brush is encompassing a boundary between selected and unselected points.
          // Previous point's position should be untouched.
          const int prev_point_i = points[i - 1];
          r_curves_cut_points[curve_i] = i;
          r_curves_cut_positions[curve_i] = deformation.positions[prev_point_i];
        }

        break;
      }
    });
  }

  /**
   * \param stroke_hash: A hash which should try to return different results for each iteration of
   * the cut. Used for "blunt scissors" mode, based on brush strength.
   */
  bool should_point_be_cut(const int point_i, const uint stroke_hash)
  {
    if (point_factors_[point_i] <= 0.0f) {
      return false;
    }

    if (brush_strength_ < 1.0f) {
      const float stroke_chance = BLI_hash_frand(stroke_hash);
      if (stroke_chance > brush_strength_) {
        return false;
      }
    }

    return true;
  }

  void cut(const Span<int> curves_cut_points,
           const Span<float3> curves_cut_positions,
           bool &r_includes_cyclic)
  {
    const OffsetIndices points_by_curve = curves_->points_by_curve();
    Array<bool> to_delete(curves_->points_num(), false);
    MutableSpan<float3> positions = curves_->positions_for_write();
    VArray<bool> cyclic = curves_->cyclic();

    curve_selection_.foreach_segment(GrainSize(256), [&](const IndexMaskSegment segment) {
      for (const int curve_i : segment) {
        const int cut_point = curves_cut_points[curve_i];
        if (cut_point < 0) {
          continue;
        }

        if (cyclic[curve_i]) {
          r_includes_cyclic = true;
          continue;
        }

        const IndexRange points = points_by_curve[curve_i];

        if (cut_point > 0) {
          const float3 cut_position = curves_cut_positions[curve_i];
          positions[points[cut_point - 1]] = cut_position;
        }

        for (const int i : IndexRange::from_begin_end(cut_point, points.size())) {
          to_delete[points[i]] = true;
        }
      }
    });

    IndexMaskMemory memory;
    curves_->remove_points(IndexMask::from_bools(to_delete.as_span(), memory), {});
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
