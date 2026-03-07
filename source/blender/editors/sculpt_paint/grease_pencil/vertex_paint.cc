/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_color.hh"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_paint.hh"

#include "grease_pencil_intern.hh"

namespace blender::ed::sculpt_paint::greasepencil {

class VertexPaintOperation : public GreasePencilStrokeOperationCommon {
  using GreasePencilStrokeOperationCommon::GreasePencilStrokeOperationCommon;

 public:
  void on_stroke_begin(const bContext &C, const InputSample &start_sample) override;
  void on_stroke_extended(const bContext &C, const InputSample &extension_sample) override;
  void on_stroke_done(const bContext & /*C*/) override {}
};

void VertexPaintOperation::on_stroke_begin(const bContext &C, const InputSample &start_sample)
{
  this->init_stroke(C, start_sample);
  this->on_stroke_extended(C, start_sample);
}

void VertexPaintOperation::on_stroke_extended(const bContext &C,
                                              const InputSample &extension_sample)
{
  const Scene &scene = *CTX_data_scene(&C);
  Paint &paint = *BKE_paint_get_active_from_context(&C);
  const Brush &brush = *BKE_paint_brush(&paint);
  const bool invert = this->is_inverted(brush);

  const bool use_selection_masking = ED_grease_pencil_any_vertex_mask_selection(
      scene.toolsettings);

  const bool do_points = do_vertex_color_points(brush);
  const bool do_fill = do_vertex_color_fill(brush);

  float color_linear[3];
  copy_v3_v3(color_linear, BKE_brush_color_get(&paint, &brush));
  const ColorGeometry4f mix_color(color_linear[0], color_linear[1], color_linear[2], 1.0f);

  this->foreach_editable_drawing(C, GrainSize(1), [&](const GreasePencilStrokeParams &params) {
    IndexMaskMemory memory;
    const IndexMask point_selection = point_mask_for_stroke_operation(
        params, use_selection_masking, memory);
    if (!point_selection.is_empty() && do_points) {
      const Array<float2> view_positions = view_positions_from_point_mask(params, point_selection);
      MutableSpan<ColorGeometry4f> vertex_colors = params.drawing.vertex_colors_for_write();

      if (invert) {
        /* Erase vertex colors. */
        point_selection.foreach_index(GrainSize(4096), [&](const int64_t point_i) {
          const float influence = brush_point_influence(
              paint, brush, view_positions[point_i], extension_sample, params.multi_frame_falloff);

          ColorGeometry4f &color = vertex_colors[point_i];
          color.a -= influence;
          color.a = math::max(color.a, 0.0f);
        });
      }
      else {
        /* Mix brush color into vertex colors by influence using alpha over. */
        point_selection.foreach_index(GrainSize(4096), [&](const int64_t point_i) {
          const float influence = brush_point_influence(
              paint, brush, view_positions[point_i], extension_sample, params.multi_frame_falloff);

          ColorGeometry4f &color = vertex_colors[point_i];
          color = math::interpolate(color, mix_color, influence);
        });
      }
    }

    const std::optional<GroupedSpan<int>> fills = params.drawing.fills();
    const IndexMask fill_selection = fill_mask_for_stroke_operation(
        params, use_selection_masking, memory);
    if (!fill_selection.is_empty() && do_fill && fills) {
      const bke::CurvesGeometry &curves = params.drawing.strokes();
      const OffsetIndices<int> points_by_curve = curves.points_by_curve();
      const Array<float2> view_positions = view_positions_from_curve_mask(params, fill_selection);
      MutableSpan<ColorGeometry4f> fill_colors = params.drawing.fill_colors_for_write();
      const VArray<int> fill_ids = *curves.attributes().lookup_or_default<int>(
          "fill_id", bke::AttrDomain::Curve, 0);

      int fill_index = 0;

      Array<int> fill_index_by_curves(curves.curves_num(), -1);
      Array<int> first_curves(curves.curves_num());
      array_utils::fill_index_range<int>(first_curves);

      for (const int curve_i : curves.curves_range()) {
        const bool is_filled = fill_ids[curve_i] != 0;
        const bool active_filled = is_filled && (fill_index_by_curves[curve_i] == -1);

        if (active_filled) {
          const Span<int> fill = (*fills)[fill_index];
          const int first_curve = fill.first();
          for (const int pos : fill.index_range()) {
            const int curve_i = fill[pos];
            fill_index_by_curves[curve_i] = fill_index;
            first_curves[curve_i] = first_curve;
          }

          fill_index++;
        }
      }

      if (invert) {
        fill_selection.foreach_index(GrainSize(1024), [&](const int64_t curve_i) {
          /* Will be `-1` if not a fill. */
          const int fill_index = fill_index_by_curves[curve_i];

          const bool is_filled = fill_index != -1;
          const bool active_filled = is_filled && (first_curves[curve_i] == curve_i);

          if (!active_filled) {
            return;
          }

          const Span<int> fill = (*fills)[fill_index];

          float influence = 0.0f;
          for (const int curve_j : fill) {
            const IndexRange points = points_by_curve[curve_j];
            const Span<float2> curve_view_positions = view_positions.as_span().slice(points);
            influence = math::max(influence,
                                  brush_fill_influence(paint,
                                                       brush,
                                                       curve_view_positions,
                                                       extension_sample,
                                                       params.multi_frame_falloff));
          }

          ColorGeometry4f &color = fill_colors[curve_i];
          color.a -= influence;
          color.a = math::max(color.a, 0.0f);

          if (fill.size() > 1) {
            index_mask::masked_fill(fill_colors, color, IndexMask::from_indices(fill, memory));
          }
        });
      }
      else {
        fill_selection.foreach_index(GrainSize(1024), [&](const int64_t curve_i) {
          /* Will be `-1` if not a fill. */
          const int fill_index = fill_index_by_curves[curve_i];

          const bool is_filled = fill_index != -1;
          const bool active_filled = is_filled && (first_curves[curve_i] == curve_i);

          if (!active_filled) {
            return;
          }

          const Span<int> fill = (*fills)[fill_index];

          float influence = 0.0f;
          for (const int curve_j : fill) {
            const IndexRange points = points_by_curve[curve_j];
            const Span<float2> curve_view_positions = view_positions.as_span().slice(points);
            influence = math::max(influence,
                                  brush_fill_influence(paint,
                                                       brush,
                                                       curve_view_positions,
                                                       extension_sample,
                                                       params.multi_frame_falloff));
          }

          ColorGeometry4f &color = fill_colors[curve_i];
          color = math::interpolate(color, mix_color, influence);

          if (fill.size() > 1) {
            index_mask::masked_fill(fill_colors, color, IndexMask::from_indices(fill, memory));
          }
        });
      }
    }

    return true;
  });
}

std::unique_ptr<GreasePencilStrokeOperation> new_vertex_paint_operation(
    const BrushStrokeMode stroke_mode)
{
  return std::make_unique<VertexPaintOperation>(stroke_mode);
}

}  // namespace blender::ed::sculpt_paint::greasepencil
