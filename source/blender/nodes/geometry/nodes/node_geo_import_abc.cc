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
  bool is_sequence = params.extract_input<bool>("Sequence");
  bool override_frame = params.extract_input<bool>("Override Frame");
  float frame = params.extract_input<float>("Override Frame F");
  float frame_offset = params.extract_input<float>("Frame Offset");
  std::string velocity_name = params.extract_input<std::string>("Velocity Attribute");
  float velocity_scale = params.extract_input<float>("Velocity Scale");

  double time = (frame + frame_offset) / 24;

  Object *self_object = const_cast<Object *>(params.self_object());
  const char *err_str = nullptr;
  GeometrySet geometry_set;
  Mesh *mesh = BKE_mesh_new_nomain(0, 0, 0, 0);
  geometry_set.replace_mesh(mesh);

#ifdef WITH_ALEMBIC
  const std::optional<std::string> path = params.ensure_absolute_path(
      params.extract_input<std::string>("Path"));
  const std::string object_path = params.extract_input<std::string>("Object Path");
  if (!path) {
    params.set_default_remaining_outputs();
    return;
  }

  CacheArchiveHandle *handle = ABC_create_handle(params.bmain(), path->c_str(), nullptr, nullptr);
  if (!handle) {
    params.error_message_add(NodeWarningType::Error, TIP_("No handle"));
    params.set_default_remaining_outputs();
    return;
  }

  CacheReader *reader = CacheReader_open_alembic_object(
      handle, nullptr, self_object, object_path.c_str(), is_sequence);

  if (!reader) {
    ABC_free_handle(handle);
    params.error_message_add(NodeWarningType::Error, TIP_("No reader/object"));
    params.set_default_remaining_outputs();
    return;
  }

  ABCReadParams read_params;
  read_params.time = time;
  read_params.velocity_name = velocity_name.c_str();
  read_params.velocity_scale = velocity_scale;
  read_params.read_flags = (MOD_MESHSEQ_READ_VERT | MOD_MESHSEQ_READ_POLY | MOD_MESHSEQ_READ_UV |
                            MOD_MESHSEQ_READ_COLOR | MOD_MESHSEQ_READ_ATTRIBUTES);

  ABC_read_geometry(reader, self_object, geometry_set, &read_params, &err_str);

  if (err_str) {
    params.error_message_add(NodeWarningType::Error, err_str);
  }
  if (geometry_set.is_empty()) {
    params.error_message_add(NodeWarningType::Error, "No geometry");
  }

  Vector<bke::GeometrySet> geometries;
  geometries.append(geometry_set);

  float mat[4][4];
  ABC_get_transform(reader, mat, read_params.time, 1);

  auto instances = std::make_unique<bke::Instances>(geometries.size());
  MutableSpan<int> handles = instances->reference_handles_for_write();
  MutableSpan<float4x4> transforms = instances->transforms_for_write();

  for (const int i : geometries.index_range()) {
    handles[i] = instances->add_reference(bke::InstanceReference{std::move(geometries[i])});
    transforms[i] = float4x4(mat);
  }

  params.set_output("Instances", bke::GeometrySet::from_instances(std::move(instances)));

  ABC_CacheReader_free(reader);
  ABC_free_handle(handle);

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
