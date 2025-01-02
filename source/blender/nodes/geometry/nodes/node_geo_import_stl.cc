/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "BKE_mesh.hh"

#include "BKE_report.hh"
#include "BLI_string.h"

#include "IO_stl.hh"

#include "node_geometry_import_cache.hh"
#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_import_stl {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("Path")
      .subtype(PROP_FILEPATH)
      .hide_label()
      .description("Path to a STL file");

  b.add_output<decl::Geometry>("Mesh");
}

static void node_geo_exec(GeoNodeExecParams params)
{
#ifdef WITH_IO_STL
  const std::string path = params.extract_input<std::string>("Path");
  if (path.empty()) {
    params.set_default_remaining_outputs();
    return;
  }

  std::shared_ptr<const geometry_import_cache::GeometryReadValue> output =
      geometry_import_cache::import_geometry_cached(
          geometry_import_cache::FileType::STL, path, [&path]() {
            STLImportParams import_params;
            STRNCPY(import_params.filepath, path.c_str());

            import_params.forward_axis = IO_AXIS_NEGATIVE_Z;
            import_params.up_axis = IO_AXIS_Y;
            import_params.use_facet_normal = false;
            import_params.use_scene_unit = false;
            import_params.global_scale = 1.0f;
            import_params.use_mesh_validate = true;

            ReportList reports;
            BKE_reports_init(&reports, RPT_STORE);
            BLI_SCOPED_DEFER([&]() { BKE_reports_free(&reports); })
            import_params.reports = &reports;

            Mesh *mesh = STL_import_mesh(&import_params);

            GeometrySet geometry = GeometrySet::from_mesh(mesh);

            auto value = std::make_unique<geometry_import_cache::GeometryReadValue>(
                geometry, import_params.reports);
            return value;
          });

  LISTBASE_FOREACH (Report *, report, &output->reports.list) {
    NodeWarningType type;
    switch (report->type) {
      case RPT_ERROR:
        type = NodeWarningType::Error;
        break;
      default:
        type = NodeWarningType::Info;
        break;
    }
    params.error_message_add(type, TIP_(report->message));
  }

  params.set_output("Mesh", output->geometry);

#else
  params.error_message_add(NodeWarningType::Error,
                           TIP_("Disabled, Blender was compiled without STL I/O"));
  params.set_default_remaining_outputs();
#endif
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, GEO_NODE_IMPORT_STL, "Import STL", NODE_CLASS_INPUT);
  ntype.enum_name_legacy = "IMPORT_STL";
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.gather_link_search_ops = search_link_ops_for_import_node;

  blender::bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_import_stl
