/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "BLI_generic_key_string.hh"
#include "BLI_listbase.h"
#include "BLI_memory_cache_file_load.hh"
#include "BLI_string.h"

#include "BKE_instances.hh"
#include "BKE_report.hh"

#include "IO_wavefront_obj.hh"

namespace blender::nodes::node_geo_import_obj {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("Path")
      .subtype(PROP_FILEPATH)
      .path_filter("*.obj")
      .hide_label()
      .description("Path to a OBJ file");

  b.add_output<decl::Geometry>("Instances");
}

class CachedLoadedOBJ : public memory_cache::CachedValue {
 public:
  Vector<bke::GeometrySet> geometries;
  Vector<geo_eval_log::NodeWarning> warnings;

  void count_memory(MemoryCounter &counter) const override
  {
    for (const bke::GeometrySet &geometry : this->geometries) {
      geometry.count_memory(counter);
    }
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
#ifdef WITH_IO_WAVEFRONT_OBJ
  const std::optional<std::string> path = params.ensure_absolute_path(
      params.extract_input<std::string>("Path"));
  if (!path) {
    params.set_default_remaining_outputs();
    return;
  }

  std::shared_ptr<const CachedLoadedOBJ> cached_value = memory_cache::get_loaded<CachedLoadedOBJ>(
      GenericStringKey{"import_obj_node"}, {StringRefNull(*path)}, [&]() {
        OBJImportParams import_params;
        STRNCPY(import_params.filepath, path->c_str());

        ReportList reports;
        BKE_reports_init(&reports, RPT_STORE);
        BLI_SCOPED_DEFER([&]() { BKE_reports_free(&reports); });
        import_params.reports = &reports;

        Vector<bke::GeometrySet> geometries;
        OBJ_import_geometries(&import_params, geometries);

        auto cached_value = std::make_unique<CachedLoadedOBJ>();
        cached_value->geometries = std::move(geometries);

        LISTBASE_FOREACH (Report *, report, &(import_params.reports)->list) {
          NodeWarningType type;
          switch (report->type) {
            case RPT_ERROR:
              type = NodeWarningType::Error;
              break;
            default:
              type = NodeWarningType::Info;
              break;
          }
          cached_value->warnings.append(geo_eval_log::NodeWarning{type, TIP_(report->message)});
        }

        return cached_value;
      });

  if (cached_value->geometries.is_empty()) {
    params.set_default_remaining_outputs();
    return;
  }
  for (const geo_eval_log::NodeWarning &warning : cached_value->warnings) {
    params.error_message_add(warning.type, warning.message);
  }

  bke::Instances *instances = new bke::Instances();
  for (GeometrySet geometry : cached_value->geometries) {
    const int handle = instances->add_reference(bke::InstanceReference{std::move(geometry)});
    instances->add_instance(handle, float4x4::identity());
  }

  params.set_output("Instances", GeometrySet::from_instances(instances));
#else
  params.error_message_add(NodeWarningType::Error,
                           TIP_("Disabled, Blender was compiled without OBJ I/O"));
  params.set_default_remaining_outputs();
#endif
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeImportOBJ", GEO_NODE_IMPORT_OBJ);
  ntype.ui_name = "Import OBJ";
  ntype.ui_description = "Import geometry from an OBJ file";
  ntype.enum_name_legacy = "IMPORT_OBJ";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;

  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_import_obj
