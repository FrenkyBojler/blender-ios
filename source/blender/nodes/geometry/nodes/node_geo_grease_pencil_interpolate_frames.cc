/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"
#include "BKE_deform.hh"
#include "BKE_grease_pencil.hh"

#include "BLI_math_geom.h"
#include "BLI_math_rotation.hh"
#include "BLI_math_vector.hh"

#include "DEG_depsgraph_query.hh"

#include "DNA_scene_types.h"

#include "GEO_foreach_geometry.hh"
#include "GEO_interpolate_curves.hh"

#include "NOD_rna_define.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_grease_pencil_interpolate_frames_cc {

enum class FlipMode : int8_t {
  None = 0,
  Flip = 1,
  Auto = 2,
};

static const EnumPropertyItem flip_mode_items[] = {
    {int(FlipMode::None), "NONE", 0, "No Flip", "Always interpolate in the same direction"},
    {int(FlipMode::Flip), "FLIP", 0, "Flip", "Always reverse the direction of interpolation"},
    {int(FlipMode::Auto),
     "AUTO",
     0,
     "Automatic",
     "Flip based on endpoint proximity to avoid crossing strokes"},
    {0, nullptr, 0, nullptr, nullptr},
};

static bool compute_auto_flip(const Span<float3> from_pos, const Span<float3> to_pos)
{
  if (from_pos.size() < 2 || to_pos.size() < 2) {
    return false;
  }
  constexpr float min_angle = DEG2RADF(15);
  const float3 &from_first = from_pos.first();
  const float3 &from_last = from_pos.last();
  const float3 &to_first = to_pos.first();
  const float3 &to_last = to_pos.last();

  if (isect_seg_seg_v2(from_first, to_first, from_last, to_last) == ISECT_LINE_LINE_CROSS) {
    if (math::angle_between(math::normalize(to_first - from_first),
                            math::normalize(to_last - from_last))
            .radian() < min_angle)
    {
      if (math::distance_squared(from_first, to_first) >=
          math::distance_squared(from_last, to_first))
      {
        return math::distance_squared(from_last, to_first) >=
               math::distance_squared(from_last, to_last);
      }
      return math::distance_squared(from_first, to_first) <
             math::distance_squared(from_first, to_last);
    }
    return true;
  }
  return math::dot(from_last - from_first, to_last - to_first) < 0.0f;
}

/**
 * Build an interpolated CurvesGeometry blending from_drawing toward to_drawing by mix_factor.
 * Curves are paired by index. Extra from-curves (when from has more than to) are copied as-is.
 *
 * Uses sample_curve_padded (control-point indices) + interpolate_curves_with_samples, exactly
 * like the GP Interpolate operator. Works correctly for all curve types without any conversion.
 */
static bke::CurvesGeometry interpolate_between_drawings(
    const GreasePencil &grease_pencil,
    const bke::greasepencil::Drawing &from_drawing,
    const bke::greasepencil::Drawing &to_drawing,
    const float mix_factor,
    const FlipMode flip_mode)
{
  const bke::CurvesGeometry &from_curves = from_drawing.strokes();
  const bke::CurvesGeometry &to_curves = to_drawing.strokes();

  const int from_num = from_curves.curves_num();
  const int to_num = to_curves.curves_num();

  if (from_num == 0 && to_num == 0) {
    return {};
  }
  if (from_num == 0) {
    return to_curves;
  }
  if (to_num == 0) {
    return from_curves;
  }

  const int paired_num = std::min(from_num, to_num);
  const int dst_curve_num = from_num;

  const OffsetIndices from_pts = from_curves.points_by_curve();
  const OffsetIndices to_pts = to_curves.points_by_curve();
  const VArray<bool> from_cyclic = from_curves.cyclic();
  const VArray<bool> to_cyclic = to_curves.cyclic();
  const Span<float3> from_positions = from_curves.positions();
  const Span<float3> to_positions = to_curves.positions();

  /* Build dst offsets and per-curve flip directions in one pass. */
  Array<int> dst_offsets(dst_curve_num + 1);
  dst_offsets[0] = 0;
  Array<bool> flip_dirs(dst_curve_num, false);

  for (const int i : IndexRange(paired_num)) {
    dst_offsets[i + 1] = dst_offsets[i] + std::max(from_pts[i].size(), to_pts[i].size());
    switch (flip_mode) {
      case FlipMode::None:
        break;
      case FlipMode::Flip:
        flip_dirs[i] = true;
        break;
      case FlipMode::Auto:
        flip_dirs[i] = compute_auto_flip(from_positions.slice(from_pts[i]),
                                         to_positions.slice(to_pts[i]));
        break;
    }
  }
  for (const int i : IndexRange(from_num - paired_num)) {
    const int ci = paired_num + i;
    dst_offsets[ci + 1] = dst_offsets[ci] + from_pts[ci].size();
  }

  const int dst_point_num = dst_offsets.last();
  bke::CurvesGeometry dst_curves(dst_point_num, dst_curve_num);
  dst_curves.offsets_for_write().copy_from(dst_offsets);
  BKE_defgroup_copy_list(&dst_curves.vertex_group_names, &grease_pencil.vertex_group_names);

  const OffsetIndices dst_pts = dst_curves.points_by_curve();

  /* sample_curve_padded produces control-point indices, which is what
   * interpolate_curves_with_samples expects — unlike sample_curve_uniform which produces
   * evaluated-segment indices that crash inside sample_catmull_rom_curve_positions_handles. */
  Array<int> from_sample_indices(dst_point_num);
  Array<int> to_sample_indices(dst_point_num);
  Array<float> from_sample_factors(dst_point_num);
  Array<float> to_sample_factors(dst_point_num);

  /* from_curve_indices[i] = i; to_curve_indices[i] = i for paired, -1 for extras. */
  Array<int> from_curve_indices(dst_curve_num);
  Array<int> to_curve_indices(dst_curve_num, -1);
  array_utils::fill_index_range(from_curve_indices.as_mutable_span());

  for (const int i : IndexRange(paired_num)) {
    to_curve_indices[i] = i;
    const IndexRange dst_range = dst_pts[i];

    if (from_pts[i].size() >= to_pts[i].size()) {
      /* dst matches from: direct mapping for from, padded for to. */
      array_utils::fill_index_range(from_sample_indices.as_mutable_span().slice(dst_range));
      from_sample_factors.as_mutable_span().slice(dst_range).fill(0.0f);
      geometry::sample_curve_padded(to_curves,
                                    i,
                                    to_cyclic[i],
                                    flip_dirs[i],
                                    to_sample_indices.as_mutable_span().slice(dst_range),
                                    to_sample_factors.as_mutable_span().slice(dst_range));
    }
    else {
      /* dst matches to: padded for from, direct mapping for to. */
      geometry::sample_curve_padded(from_curves,
                                    i,
                                    from_cyclic[i],
                                    flip_dirs[i],
                                    from_sample_indices.as_mutable_span().slice(dst_range),
                                    from_sample_factors.as_mutable_span().slice(dst_range));
      array_utils::fill_index_range(to_sample_indices.as_mutable_span().slice(dst_range));
      to_sample_factors.as_mutable_span().slice(dst_range).fill(0.0f);
    }
  }

  /* Extra from-only curves: to_curve_indices stays -1, direct copy via fill_index_range. */
  for (const int i : IndexRange(paired_num, from_num - paired_num)) {
    const IndexRange dst_range = dst_pts[i];
    array_utils::fill_index_range(from_sample_indices.as_mutable_span().slice(dst_range));
    from_sample_factors.as_mutable_span().slice(dst_range).fill(0.0f);
  }

  IndexMaskMemory memory;
  geometry::interpolate_curves_with_samples(from_curves,
                                            to_curves,
                                            from_curve_indices,
                                            to_curve_indices,
                                            from_sample_indices,
                                            to_sample_indices,
                                            from_sample_factors,
                                            to_sample_factors,
                                            IndexMask(IndexRange(dst_curve_num)),
                                            mix_factor,
                                            dst_curves,
                                            memory);

  return dst_curves;
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_default_layout();
  b.add_input<decl::Geometry>("Grease Pencil"_ustr)
      .supported_type(GeometryComponent::Type::GreasePencil)
      .align_with_previous()
      .description("Grease Pencil strokes to interpolate between adjacent keyframes");
  b.add_output<decl::Geometry>("Grease Pencil"_ustr)
      .propagate_all_geometry()
      .align_with_previous();
  b.add_input<decl::Float>("Factor Offset"_ustr)
      .default_value(0.0f)
      .min(-2.0f)
      .max(2.0f)
      .structure_type(StructureType::Field)
      .description(
          "Added to the auto-computed blend factor (frame position between keyframes). "
          "Values outside 0-1 produce overshoot. "
          "Connect a curve, noise, or any expression to offset or shape the motion");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Grease Pencil"_ustr);
  const Field<float> factor_offset_field = params.extract_input<Field<float>>(
      "Factor Offset"_ustr);
  /* Evaluate the field with a trivial 1-element context. Works correctly for fields that don't
   * depend on geometry attributes (e.g. Scene Time -> Noise -> here). */
  float factor_offset;
  {
    FieldContext context;
    fn::FieldEvaluator evaluator{context, 1};
    evaluator.add(factor_offset_field);
    evaluator.evaluate();
    factor_offset = evaluator.get_evaluated<float>(0)[0];
  }

  const Depsgraph *depsgraph = params.depsgraph();
  if (!depsgraph) {
    params.set_output("Grease Pencil"_ustr, std::move(geometry_set));
    return;
  }

  const Scene *scene = DEG_get_evaluated_scene(depsgraph);
  if (!scene) {
    params.set_output("Grease Pencil"_ustr, std::move(geometry_set));
    return;
  }

  const int current_frame = scene->r.cfra;

  geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry) {
    if (GreasePencil *grease_pencil = geometry.get_grease_pencil_for_write()) {
      using namespace bke::greasepencil;

      for (const int layer_index : grease_pencil->layers().index_range()) {
        const Layer &layer = grease_pencil->layer(layer_index);

        const Span<int> sorted_keys = layer.sorted_keys();
        if (sorted_keys.size() < 2) {
          continue;
        }

        /* Find the last valid keyframe at or before current_frame. */
        const Layer::SortedKeysIterator prev_it = layer.sorted_keys_iterator_at(current_frame);
        if (!prev_it) {
          continue;
        }

        const int from_frame = *prev_it;
        const GreasePencilFrame *from_gp_frame = layer.frame_at(from_frame);
        if (!from_gp_frame || from_gp_frame->is_end()) {
          continue;
        }

        /* No interpolation needed when the current frame is exactly on a keyframe. */
        if (from_frame == current_frame) {
          continue;
        }

        /* Find the next valid keyframe after current_frame. */
        const Layer::SortedKeysIterator next_it = std::next(prev_it);
        if (next_it == sorted_keys.end()) {
          continue;
        }

        const int to_frame = *next_it;
        const GreasePencilFrame *to_gp_frame = layer.frame_at(to_frame);
        if (!to_gp_frame || to_gp_frame->is_end()) {
          continue;
        }

        const float auto_factor = float(current_frame - from_frame) / float(to_frame - from_frame);
        const float mix_factor = auto_factor + factor_offset;

        const Drawing *from_drawing = grease_pencil->get_drawing_at(layer, from_frame);
        const Drawing *to_drawing = grease_pencil->get_drawing_at(layer, to_frame);
        if (!from_drawing || !to_drawing) {
          continue;
        }

        /* The eval_drawing is from_drawing (holding since from_frame).
         * Replacing its strokes with the interpolated result is safe because the
         * interpolation is fully computed before the assignment. */
        Drawing *current_drawing = grease_pencil->get_eval_drawing(layer);
        if (!current_drawing) {
          continue;
        }

        const FlipMode flip_mode = FlipMode(params.node().custom1);
        bke::CurvesGeometry result = interpolate_between_drawings(
            *grease_pencil, *from_drawing, *to_drawing, mix_factor, flip_mode);

        current_drawing->strokes_for_write() = std::move(result);
        current_drawing->tag_topology_changed();
      }
    }
  });

  params.set_output("Grease Pencil"_ustr, std::move(geometry_set));
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "flip_mode", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = int(FlipMode::Auto);
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(srna,
                    "flip_mode",
                    "Flip Mode",
                    "How to handle stroke direction when interpolating",
                    flip_mode_items,
                    NOD_inline_enum_accessors(custom1));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeGreasePencilInterpolateFrames"_ustr);
  ntype.ui_name = "Interpolate Grease Pencil Frames";
  ntype.ui_description =
      "Automatically interpolate Grease Pencil strokes between adjacent keyframes";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  ntype.draw_buttons = node_layout;
  ntype.default_width = bke::NodeWidth::_240;
  bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_grease_pencil_interpolate_frames_cc
