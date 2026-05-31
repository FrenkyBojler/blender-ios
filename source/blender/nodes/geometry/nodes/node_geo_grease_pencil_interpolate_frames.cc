/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"
#include "BKE_deform.hh"
#include "BKE_grease_pencil.hh"

#include "BLI_array_utils.hh"
#include "BLI_math_base.hh"
#include "BLI_offset_indices.hh"

#include "DEG_depsgraph_query.hh"

#include "DNA_scene_types.h"

#include "GEO_foreach_geometry.hh"
#include "GEO_interpolate_curves.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_grease_pencil_interpolate_frames_cc {

/**
 * Build an interpolated CurvesGeometry blending from_drawing toward to_drawing by mix_factor.
 * Curves are paired by index. Extra from-curves (when from has more than to) are preserved by
 * self-interpolating at factor 0.
 */
static bke::CurvesGeometry interpolate_between_drawings(
    const GreasePencil &grease_pencil,
    const bke::greasepencil::Drawing &from_drawing,
    const bke::greasepencil::Drawing &to_drawing,
    const float mix_factor)
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
  /* Output keeps from_drawing's curve count; extra to-curves are ignored. */
  const int dst_curve_num = from_num;

  const OffsetIndices from_pts = from_curves.points_by_curve();
  const OffsetIndices to_pts = to_curves.points_by_curve();

  /* Paired curves get max(from, to) points so both can be fully sampled. */
  Array<int> dst_offsets(dst_curve_num + 1);
  dst_offsets[0] = 0;
  for (const int i : IndexRange(paired_num)) {
    dst_offsets[i + 1] = dst_offsets[i] + std::max(from_pts[i].size(), to_pts[i].size());
  }
  /* Extra from-only curves keep their original point count. */
  for (const int i : IndexRange(from_num - paired_num)) {
    const int ci = paired_num + i;
    dst_offsets[ci + 1] = dst_offsets[ci] + from_pts[ci].size();
  }

  const int dst_point_num = dst_offsets.last();
  bke::CurvesGeometry dst_curves(dst_point_num, dst_curve_num);
  dst_curves.offsets_for_write().copy_from(dst_offsets);

  /* Vertex group names are needed by the attribute interpolation internals. */
  BKE_defgroup_copy_list(&dst_curves.vertex_group_names, &grease_pencil.vertex_group_names);

  IndexMaskMemory memory;

  if (paired_num > 0) {
    Array<int> from_indices(paired_num);
    Array<int> to_indices(paired_num);
    Array<bool> flip_dirs(paired_num, false);
    array_utils::fill_index_range(from_indices.as_mutable_span());
    array_utils::fill_index_range(to_indices.as_mutable_span());

    geometry::interpolate_curves(from_curves,
                                 to_curves,
                                 from_indices,
                                 to_indices,
                                 IndexMask(IndexRange(paired_num)),
                                 flip_dirs,
                                 mix_factor,
                                 dst_curves,
                                 memory);
  }

  /* Self-interpolate extra from-curves so their attributes are filled in dst. */
  const int extra_num = from_num - paired_num;
  if (extra_num > 0) {
    Array<int> extra_from(extra_num);
    Array<int> extra_to(extra_num);
    Array<bool> extra_flip(extra_num, false);
    for (const int i : IndexRange(extra_num)) {
      extra_from[i] = paired_num + i;
      extra_to[i] = paired_num + i;
    }

    geometry::interpolate_curves(from_curves,
                                 from_curves,
                                 extra_from,
                                 extra_to,
                                 IndexMask(IndexRange(paired_num, extra_num)),
                                 extra_flip,
                                 0.0f,
                                 dst_curves,
                                 memory);
  }

  return dst_curves;
}

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Geometry>("Grease Pencil"_ustr)
      .supported_type(GeometryComponent::Type::GreasePencil)
      .align_with_previous()
      .description("Grease Pencil strokes to interpolate between adjacent keyframes");
  b.add_output<decl::Geometry>("Grease Pencil"_ustr)
      .propagate_all_geometry()
      .align_with_previous();
  b.add_input<decl::Float>("Shift"_ustr)
      .default_value(0.0f)
      .min(-1.0f)
      .max(1.0f)
      .description("Bias added to the computed blend factor. "
                   "Positive values push toward the next keyframe, negative toward the previous");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Grease Pencil"_ustr);
  const float shift = params.extract_input<float>("Shift"_ustr);

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

        /* Linear blend factor based on where the current frame sits in [from, to]. */
        const float base_factor = float(current_frame - from_frame) /
                                  float(to_frame - from_frame);
        const float mix_factor = math::clamp(base_factor + shift, 0.0f, 1.0f);

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

        bke::CurvesGeometry result = interpolate_between_drawings(
            *grease_pencil, *from_drawing, *to_drawing, mix_factor);

        current_drawing->strokes_for_write() = std::move(result);
        current_drawing->tag_topology_changed();
      }
    }
  });

  params.set_output("Grease Pencil"_ustr, std::move(geometry_set));
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
  ntype.default_width = bke::NodeWidth::_240;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_grease_pencil_interpolate_frames_cc
