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
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Grease Pencil"_ustr);
  const int target_frame = params.extract_input<int>("Frame"_ustr);

  const GreasePencil *gp_eval = geometry_set.get_grease_pencil();
  if (gp_eval == nullptr) {
    params.set_default_remaining_outputs();
    return;
  }

  /* Look up the original unmodified datablock in BMain by name. The name is preserved through
   * depsgraph eval copies, so this reliably bypasses whatever the modifier stack has written
   * into the evaluated copy (e.g. the Time modifier rewrites frames_for_write on eval).
   * If not found the geometry was synthesized by geo nodes with no BMain backing — use as-is. */
  const GreasePencil *gp_orig = id_cast<const GreasePencil *>(
      BKE_libblock_find_name(params.bmain(), ID_GP, gp_eval->id.name + 2));
  if (gp_orig == nullptr) {
    gp_orig = gp_eval;
  }

  using namespace bke::greasepencil;

  GreasePencil *gp_copy = BKE_grease_pencil_copy_for_eval(gp_orig);

  int input_frames = 0;
  for (const Layer *layer : gp_copy->layers()) {
    input_frames += int(layer->frames().size());
  }

  /* Remap each layer to a fixed key (0) so the copy is self-contained and safe to use
   * as an instance. Feed the original GP as a fixed external input to any Repeat Zone,
   * not as the loopback, so the original frame map is intact on every iteration. */
  for (Layer *layer : gp_copy->layers_for_write()) {
    const int drawing_index = layer->drawing_index_at(target_frame);
    Map<int, GreasePencilFrame> new_frames;
    if (drawing_index != -1) {
      GreasePencilFrame entry{};
      entry.drawing_index = drawing_index;
      new_frames.add(0, entry);
    }
    layer->frames_for_write() = std::move(new_frames);
    layer->tag_frames_map_keys_changed();
  }
  gp_copy->runtime->eval_frame = 0;

  int output_frames = 0;
  for (const Layer *layer : gp_copy->layers()) {
    output_frames += int(layer->frames().size());
  }

  GeometrySet output;
  auto &component = output.get_component_for_write<bke::GreasePencilComponent>();
  component.replace(gp_copy, bke::GeometryOwnershipType::Owned);

  params.error_message_add(
      NodeWarningType::Info,
      fmt::format("Using original data from '{}' — {} input frame(s) → {} output frame(s)",
                  gp_orig->id.name + 2,
                  input_frames,
                  output_frames));

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
