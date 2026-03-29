/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_image.hh"

#include "BLI_fileops.h"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_import_image {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("Path")
      .subtype(PROP_FILEPATH)
      .path_filter(
          "*.bmp;*.png;*.exr;*.hdr;*.tga;*.tif;*.jpg;*.jp2;*.j2c;*.dpx;*.cin;*.webp;*.avif;*.psd") //todo movies
      .optional_label()
      .description("Path to a image file");

  b.add_output<decl::Image>("Image");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const std::optional<std::string> path = params.ensure_absolute_path(
      params.extract_input<std::string>("Path"_ustr));
  if (!path) {
    params.set_default_remaining_outputs();
    return;
  }

  Image *image;
  image = BKE_image_load_exists(params.bmain(), path->c_str());
  if (!image) {
    params.set_default_remaining_outputs();
    params.error_message_add(NodeWarningType::Error, TIP_("Image path not found"));
    return;
  }

  params.set_output("Image"_ustr, reinterpret_cast<Image *>(image));
  printf("output");
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeImportImage");
  ntype.ui_name = "Import Image";
  ntype.ui_description = "Import an image from a file";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_import_image
