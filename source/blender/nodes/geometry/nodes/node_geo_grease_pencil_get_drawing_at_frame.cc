/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_grease_pencil.hh"
#include "BKE_lib_id.hh"

#include "DNA_ID_enums.h"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_grease_pencil_get_drawing_at_frame_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Geometry>("Grease Pencil"_ustr)
      .supported_type(GeometryComponent::Type::GreasePencil)
      .align_with_previous()
      .description("Grease Pencil geometry to get the drawing from");
  b.add_output<decl::Geometry>("Grease Pencil"_ustr)
      .propagate_all_geometry()
      .align_with_previous();
  b.add_input<decl::Int>("Frame"_ustr)
      .default_value(1)
      .description("Scene frame number to get the Grease Pencil drawing at");
  b.add_input<decl::Bool>("Use Original Data"_ustr)
      .default_value(true)
      .description(
          "Read from the original unmodified datablock, bypassing any modifiers. "
          "Disable when chaining multiple nodes so each node works on the result of the previous");
  b.add_input<decl::Bool>("Selection"_ustr)
      .default_value(true)
      .hide_value()
      .evaluated_geometry_field()
      .description("Select which layers to include in the output");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Grease Pencil"_ustr);
  const int target_frame = params.extract_input<int>("Frame"_ustr);
  const bool use_original = params.extract_input<bool>("Use Original Data"_ustr);

  const GreasePencil *gp_eval = geometry_set.get_grease_pencil();
  if (gp_eval == nullptr) {
    params.set_default_remaining_outputs();
    return;
  }

  const GreasePencil *gp_orig = gp_eval;
  if (use_original) {
    /* Look up the original unmodified datablock in BMain by name. The name is preserved through
     * depsgraph eval copies, so this reliably bypasses whatever the modifier stack has written
     * into the evaluated copy (e.g. the Time modifier rewrites frames_for_write on eval).
     * If not found the geometry was synthesized by geo nodes with no BMain backing — use as-is. */
    if (const GreasePencil *found = id_cast<const GreasePencil *>(
            BKE_libblock_find_name(params.bmain(), ID_GP, gp_eval->id.name + 2)))
    {
      gp_orig = found;
    }
  }

  using namespace bke::greasepencil;

  GreasePencil *gp_copy = BKE_grease_pencil_copy_for_eval(gp_orig);

  const bke::GreasePencilFieldContext field_context{*gp_copy};
  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection"_ustr);
  FieldEvaluator evaluator{field_context, gp_copy->layers().size()};
  evaluator.set_selection(selection_field);
  evaluator.evaluate();
  const IndexMask layer_selection = evaluator.get_evaluated_selection_as_mask();

  /* Remap selected layers to a fixed key (0) so the copy is self-contained and safe to use
   * as an instance. Unselected layers are cleared. Feed the original GP as a fixed external
   * input to any Repeat Zone, not as the loopback, so the original frame map is intact on
   * every iteration. */
  const Span<Layer *> layers = gp_copy->layers_for_write();
  layer_selection.foreach_index([&](const int i) {
    Layer &layer = *layers[i];
    const int drawing_index = layer.drawing_index_at(target_frame);
    Map<int, GreasePencilFrame> new_frames;
    if (drawing_index != -1) {
      GreasePencilFrame entry{};
      entry.drawing_index = drawing_index;
      new_frames.add(0, entry);
    }
    layer.frames_for_write() = std::move(new_frames);
    layer.tag_frames_map_keys_changed();
  });
  gp_copy->runtime->eval_frame = 0;

  GeometrySet output;
  auto &component = output.get_component_for_write<bke::GreasePencilComponent>();
  component.replace(gp_copy, bke::GeometryOwnershipType::Owned);

  params.set_output("Grease Pencil"_ustr, std::move(output));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeGreasePencilGetDrawingAtFrame"_ustr);
  ntype.ui_name = "Get Drawing at Frame";
  ntype.ui_description =
      "Get the Grease Pencil drawing at a specific frame, reading from the original stored data "
      "before any modifiers are applied";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_grease_pencil_get_drawing_at_frame_cc
