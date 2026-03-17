/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "BLI_generic_key_string.hh"
#include "BLI_listbase.h"
#include "BLI_memory_cache_file_load.hh"
#include "BLI_string.h"

#include "BKE_instances.hh"
#include "BKE_mesh.h"
#include "BKE_report.hh"

#ifdef WITH_ALEMBIC
#  include "ABC_alembic.h"
#endif

namespace blender::nodes::node_geo_import_abc {

enum MeshSeqCacheModifierReadFlag {
  MOD_MESHSEQ_READ_VERT = (1 << 0),
  MOD_MESHSEQ_READ_POLY = (1 << 1),
  MOD_MESHSEQ_READ_UV = (1 << 2),
  MOD_MESHSEQ_READ_COLOR = (1 << 3),
  MOD_MESHSEQ_INTERPOLATE_VERTICES = (1 << 4),
  MOD_MESHSEQ_READ_ATTRIBUTES = (1 << 5),
};

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
  b.add_input<decl::String>("Object Path").optional_label().description("Object Path");

  {
    auto &p = b.add_panel("Time"_ustr).default_closed(false);
    p.add_input<decl::Bool>("Sequence");
    p.add_input<decl::Float>("Time");
  }
  {
    auto &p = b.add_panel("Velocity"_ustr).default_closed(true);
    p.add_input<decl::String>("Velocity Attribute").default_value(".velocity").optional_label();
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
  bool is_sequence = params.extract_input<bool>("Sequence");
  double time = params.extract_input<float>("Time");
  std::string velocity_name = params.extract_input<std::string>("Velocity Attribute");
  float velocity_scale = params.extract_input<float>("Velocity Scale");

#ifdef WITH_ALEMBIC
  const std::optional<std::string> path = params.ensure_absolute_path(
      params.extract_input<std::string>("Path"));
  const std::string object_path = params.extract_input<std::string>("Object Path");
  if (!path) {
    params.set_default_remaining_outputs();
    return;
  }

  ABCReadParams read_params;
  read_params.time = time;
  read_params.velocity_name = velocity_name.c_str();
  read_params.velocity_scale = velocity_scale;
  read_params.read_flags = (MOD_MESHSEQ_READ_VERT | MOD_MESHSEQ_READ_POLY | MOD_MESHSEQ_READ_UV |
                            MOD_MESHSEQ_READ_COLOR | MOD_MESHSEQ_READ_ATTRIBUTES);

  Vector<bke::GeometrySet> geometries;
  Vector<float4x4> f4x4s;
  Vector<int> parent_ids;
  Vector<int> parent_counts;

  ABC_geo_and_trans(params.bmain(),
                    path->c_str(),
                    object_path.c_str(),
                    &read_params,
                    geometries,
                    f4x4s,
                    parent_ids,
                    parent_counts);

  if (geometries.size() == 0) {
    params.error_message_add(NodeWarningType::Error, "No geometry");
    params.set_default_remaining_outputs();
    return;
  }

  auto instances = std::make_unique<bke::Instances>(geometries.size());
  MutableSpan<int> handles = instances->reference_handles_for_write();
  MutableSpan<float4x4> transforms = instances->transforms_for_write();

  for (const int i : geometries.index_range()) {
    handles[i] = instances->add_reference(bke::InstanceReference{std::move(geometries[i])});
    transforms[i] = f4x4s[i];
  }

  MutableAttributeAccessor attributes = instances->attributes_for_write();
  attributes.add<int>("parent_index",
                      bke::AttrDomain::Instance,
                      bke::AttributeInitVArray(VArray<int>::from_span(parent_ids)));

  attributes.add<int>("parent_count",
                      bke::AttrDomain::Instance,
                      bke::AttributeInitVArray(VArray<int>::from_span(parent_counts)));

  params.set_output("Instances", bke::GeometrySet::from_instances(std::move(instances)));

#else
  params.error_message_add(NodeWarningType::Error,
                           TIP_("Disabled, Blender was compiled without Alembic"));
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
