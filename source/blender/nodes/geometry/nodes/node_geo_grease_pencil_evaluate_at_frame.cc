/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_grease_pencil.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_grease_pencil_evaluate_at_frame_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Geometry>("Grease Pencil"_ustr)
      .supported_type(GeometryComponent::Type::GreasePencil)
      .align_with_previous()
      .description("Grease Pencil geometry to evaluate at a specific frame");
  b.add_output<decl::Geometry>("Grease Pencil"_ustr)
      .propagate_all_geometry()
      .align_with_previous();
  b.add_input<decl::Int>("Frame"_ustr)
      .default_value(1)
      .description("Scene frame number to evaluate the Grease Pencil drawings at");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Grease Pencil"_ustr);
  const int target_frame = params.extract_input<int>("Frame"_ustr);

  if (GreasePencil *grease_pencil = geometry_set.get_grease_pencil_for_write()) {
    using namespace bke::greasepencil;
    const int current_eval_frame = grease_pencil->runtime->eval_frame;

    for (Layer *layer : grease_pencil->layers_for_write()) {
      int drawing_index = layer->drawing_index_at(target_frame);
      if (drawing_index == -1) {
        /* The frame map may already have been remapped by a previous Repeat Zone
         * iteration (it contains only {current_eval_frame → index}). When
         * current_eval_frame > target_frame there is no key ≤ target_frame so the
         * lookup returns -1. Fall back to current_eval_frame, which holds the
         * target drawing stored by the previous iteration. */
        drawing_index = layer->drawing_index_at(current_eval_frame);
      }

      /* Remap the layer's frame map so the current eval frame points to the drawing
       * at target_frame. This is the same mechanism used by the GP Time modifier. */
      Map<int, GreasePencilFrame> new_frames;
      if (drawing_index != -1) {
        GreasePencilFrame entry;
        entry.drawing_index = drawing_index;
        new_frames.add(current_eval_frame, entry);
      }
      layer->frames_for_write() = std::move(new_frames);
      layer->tag_frames_map_keys_changed();
    }
  }

  params.set_output("Grease Pencil"_ustr, std::move(geometry_set));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeGreasePencilEvaluateAtFrame"_ustr);
  ntype.ui_name = "Evaluate at Frame";
  ntype.ui_description =
      "Evaluate the Grease Pencil drawings at a specific frame instead of the current scene frame";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_grease_pencil_evaluate_at_frame_cc
