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

    Array<bool> points_in_stroke(curves_->points_num(), false);

    if (falloff_shape == PAINT_FALLOFF_SHAPE_TUBE) {
      this->find_projected_points_in_stroke_with_symmetry(points_in_stroke);
    }
    else if (falloff_shape == PAINT_FALLOFF_SHAPE_SPHERE) {
      this->find_spherical_points_in_stroke_with_symmetry(points_in_stroke);
    }
    else {
      BLI_assert_unreachable();
    }

    bool includes_cyclic = false;
    this->cut(points_in_stroke, includes_cyclic);
    if (includes_cyclic) {
      report_cyclic_not_supported(stroke_extension.reports);
    }

    curves_->tag_topology_changed();
    DEG_id_tag_update(&curves_id_->id, ID_RECALC_GEOMETRY);
    WM_main_add_notifier(NC_GEOM | ND_DATA, &curves_id_->id);
    ED_region_tag_redraw(ctx_.region);
  }

  void find_projected_points_in_stroke_with_symmetry(MutableSpan<bool> r_points_in_stroke)
  {
    const Vector<float4x4> symmetry_brush_transforms = get_symmetry_brush_transforms(
        eCurvesSymmetryType(curves_id_->symmetry));
    for (const float4x4 &brush_transform : symmetry_brush_transforms) {
      this->find_projected_points_in_stroke(brush_transform, r_points_in_stroke);
    }
  }

  void find_projected_points_in_stroke(const float4x4 &brush_transform,
                                       MutableSpan<bool> r_points_in_stroke)
  {
    const float4x4 brush_transform_inv = math::invert(brush_transform);

    const float brush_radius_re = brush_radius_base_re_ * brush_radius_factor_;
    const float brush_radius_sq_re = pow2f(brush_radius_re);

    const float4x4 projection = ED_view3d_ob_project_mat_get(ctx_.rv3d, object_);

    const bke::crazyspace::GeometryDeformation deformation =
        bke::crazyspace::get_evaluated_curves_deformation(*ctx_.depsgraph, *object_);
    const OffsetIndices points_by_curve = curves_->points_by_curve();

    curve_selection_.foreach_index(GrainSize(256), [&](const int curve_i) {
      const IndexRange points = points_by_curve[curve_i];
      for (const int point_i : points) {
        const float3 &pos_cu = math::transform_point(brush_transform_inv,
                                                     deformation.positions[point_i]);
        const float2 pos_re = ED_view3d_project_float_v2_m4(ctx_.region, pos_cu, projection);
        const float dist_to_brush_sq_re = math::distance_squared(pos_re, brush_pos_re_);
        if (dist_to_brush_sq_re > brush_radius_sq_re) {
          continue;
        }

        const bool in_stroke = point_factors_[point_i] > 0.0f;
        r_points_in_stroke[point_i] |= in_stroke;
      }
    });
  }

  void find_spherical_points_in_stroke_with_symmetry(MutableSpan<bool> r_points_in_stroke)
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
                                            r_points_in_stroke);
    }
  }

  void find_spherical_points_in_stroke(const float3 &brush_pos_cu,
                                       const float brush_radius_cu,
                                       MutableSpan<bool> r_points_in_stroke)
  {
    const float brush_radius_sq_cu = pow2f(brush_radius_cu);
    const bke::crazyspace::GeometryDeformation deformation =
        bke::crazyspace::get_evaluated_curves_deformation(*ctx_.depsgraph, *object_);
    const OffsetIndices points_by_curve = curves_->points_by_curve();

    curve_selection_.foreach_index(GrainSize(256), [&](const int curve_i) {
      const IndexRange points = points_by_curve[curve_i];
      for (const int point_i : points) {
        const float3 &pos_cu = deformation.positions[point_i];
        const float dist_to_brush_sq_cu = math::distance_squared(pos_cu, brush_pos_cu);
        if (dist_to_brush_sq_cu > brush_radius_sq_cu) {
          continue;
        }

        const bool in_stroke = point_factors_[point_i] > 0.0f;
        r_points_in_stroke[point_i] |= in_stroke;
      }
    });
  }

  void cut(const Span<bool> points_in_stroke, bool &r_includes_cyclic)
  {
    const OffsetIndices points_by_curve = curves_->points_by_curve();
    Array<bool> to_delete(curves_->points_num(), false);
    MutableSpan<float3> positions = curves_->positions_for_write();
    VArray<bool> cyclic = curves_->cyclic();

    curve_selection_.foreach_segment(GrainSize(256), [&](const IndexMaskSegment segment) {
      for (const int curve_i : segment) {
        if (cyclic[curve_i]) {
          r_includes_cyclic = true;
          continue;
        }

        const IndexRange points = points_by_curve[curve_i];
        bool is_cutting = false;
        for (const int i : IndexRange(points.size())) {
          const int point_i = points[i];
          if (is_cutting) {
            // Delete remaining points on the curve.
            to_delete[point_i] = true;
          }
          else if (points_in_stroke[point_i]) {
            // Start a cut at point i.
            is_cutting = true;

            if (i > 0) {
              // Shorten the curve up to the point i, moving it to the cut brush boundary.
              // TODO: cache unchanging data (mainly brush info) + handle symmetry axes (could be
              // merged into the prior "tagging" loop? not sure yet)
              const eBrushFalloffShape falloff_shape = eBrushFalloffShape(brush_->falloff_shape);
              if (falloff_shape == PAINT_FALLOFF_SHAPE_TUBE) {
                const float brush_radius_re = brush_radius_base_re_ * brush_radius_factor_;

                const float3 intersection = find_projected_cut_boundary(
                    positions[points[i - 1]], positions[point_i], brush_pos_re_, brush_radius_re);
                positions[point_i] = intersection;
              }
              else if (falloff_shape == PAINT_FALLOFF_SHAPE_SPHERE) {
                float3 brush_pos_wo;
                ED_view3d_win_to_3d(ctx_.v3d,
                                    ctx_.region,
                                    math::transform_point(transforms_.curves_to_world,
                                                          self_->brush_3d_.position_cu),
                                    brush_pos_re_,
                                    brush_pos_wo);
                const float3 brush_pos_cu = math::transform_point(transforms_.world_to_curves,
                                                                  brush_pos_wo);
                const float brush_radius_cu = self_->brush_3d_.radius_cu * brush_radius_factor_;

                const float3 intersection = find_spherical_cut_boundary(
                    positions[points[i - 1]], positions[point_i], brush_pos_cu, brush_radius_cu);
                positions[point_i] = intersection;
              }
              else {
                BLI_assert_unreachable();
              }
            }
            else {
              // If the curve root is included in the cut, then remove the entire curve instead of
              // shortening.
              to_delete[point_i] = true;
            }
          }
        }
      }
    });

    IndexMaskMemory memory;
    curves_->remove_points(IndexMask::from_bools(to_delete.as_span(), memory), {});
  }

  float3 find_projected_cut_boundary(const float3 &point_outside_cu,
                                     const float3 &point_inside_cu,
                                     const float2 &brush_pos_re,
                                     const float brush_radius_re)
  {
    const float4x4 projection = ED_view3d_ob_project_mat_get(ctx_.rv3d, object_);

    const float2 point_outside_re = ED_view3d_project_float_v2_m4(
        ctx_.region, point_outside_cu, projection);
    const float2 point_inside_re = ED_view3d_project_float_v2_m4(
        ctx_.region, point_inside_cu, projection);

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
