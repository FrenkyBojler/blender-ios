/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "BLI_generic_key_string.hh"
#include "BLI_listbase.h"
#include "BLI_memory_cache_file_load.hh"
#include "BLI_string.h"

#include "BKE_instances.hh"
#include "BKE_report.hh"

#ifdef WITH_ALEMBIC
#  include "ABC_alembic.h"
#endif

namespace blender::nodes::node_geo_import_abc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_output<decl::Geometry>("Instances");
  b.add_input<decl::String>("Path")
      .subtype(PROP_FILEPATH)
      .path_filter("*.abc")
      .optional_label()
      .description("Path to an Alembic file");
  {
    auto &p = b.add_panel("Time").default_closed(false);
    p.add_input<decl::Bool>("Sequence");
    p.add_input<decl::Bool>("Override Frame");
    p.add_input<decl::Float>("Override Frame F");
    p.add_input<decl::Float>("Frame Offset");
  }
  {
    auto &p = b.add_panel("Velocity").default_closed(true);
    p.add_input<decl::String>("Velocity Attribute").default_value(".velocity").optional_label();
    p.add_input<decl::Int>("Velocity Unit");
    p.add_input<decl::Float>("Velocity Scale").default_value(1.0f);
  }
}

class LoadAbcCache : public memory_cache::CachedValue {
 public:
  GeometrySet geometry;
  Vector<geo_eval_log::NodeWarning> warnings;

  void count_memory(MemoryCounter &counter) const override
  {
    this->geometry.count_memory(counter);
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
#ifdef WITH_ALEMBIC
  /*

    for (const geo_eval_log::NodeWarning &warning : cached_value->warnings) {
      params.error_message_add(warning.type, warning.message);
    }

    params.set_output("Instances", cached_value->geometry);
*/
  else params.error_message_add(NodeWarningType::Error,
                                TIP_("Disabled, Blender was compiled without ABC I/O"));
  params.set_default_remaining_outputs();
#endif
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeImportABC", GEO_NODE_IMPORT_ABC);
  ntype.ui_name = "Import ABC";
  ntype.ui_description = "Import geometry from an Alembic file";
  ntype.enum_name_legacy = "IMPORT_ABC";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_import_abc
