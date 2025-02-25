/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"
#include "BLI_math_geom.h"
#include "BLI_task.hh"
#include "BLI_utildefines.h"

#include "BKE_curves.hh"
#include "BKE_customdata.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_mapping.hh"
#include "BKE_mesh_tangent.hh"
#include "BKE_type_conversions.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_mesh_texture_coordinate_cc {

/* Socket names. */
const char OUTPUT_UV_MAP[7] = "UV Map";

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Vector>(OUTPUT_UV_MAP)
      .field_source()
      .description("UV coordinates for the active UV map layer");
}

class MeshTextureCoordinateFieldInput final : public bke::MeshFieldInput {
 private:
 public:
  MeshTextureCoordinateFieldInput()
      : bke::MeshFieldInput(CPPType::get<float2>(), "MeshTextureCoordinate")
  {
    category_ = Category::Generated;
  }

  virtual GVArray get_varray_for_context(const Mesh &mesh,
                                         AttrDomain domain,
                                         const IndexMask & /*mask*/) const
  {

    /* Active UV layer fetch */
    const CustomData *corner_data = &mesh.corner_data;
    int layer_index = CustomData_get_layer_index(corner_data, CD_PROP_FLOAT2);
    int active_layer = CustomData_get_active_layer(corner_data, CD_PROP_FLOAT2);
    const float2 *active_uv_data = static_cast<const float2 *>(
        corner_data->layers[active_layer + layer_index].data);

    Span<float2> active_uvs(active_uv_data, mesh.corners_num);
    VArray<float2> uv_coords = VArray<float2>::ForSpan(active_uvs);

    if (domain == AttrDomain::Corner) {
      return uv_coords;
    }
    return mesh.attributes().adapt_domain(uv_coords, AttrDomain::Corner, domain);
  }

  uint64_t hash() const override
  {
    /* Random constant hash. */
    return 0xb71bfcec528e62b3;
  }

  std::optional<AttrDomain> preferred_domain(const Mesh & /*mesh*/) const final
  {
    return AttrDomain::Corner;
  }

  bool is_equal_to(const fn::FieldNode &other) const override
  {
    return dynamic_cast<const MeshTextureCoordinateFieldInput *>(&other) != nullptr;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  Field<float2> uv_map_field{std::make_shared<MeshTextureCoordinateFieldInput>()};

  const bke::DataTypeConversions &conversions = bke::get_implicit_type_conversions();
  const CPPType &float3_type = CPPType::get<float3>();
  Field<float3> field_uv_as_vec = conversions.try_convert(uv_map_field, float3_type);

  params.set_output(OUTPUT_UV_MAP, std::move(field_uv_as_vec));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(
      &ntype, "GeometryNodeInputMeshTextureCoordinate", GEO_NODE_MESH_TEXTURE_COORDINATE);
  ntype.ui_name = "Texture Coordinate";
  ntype.ui_description = "UV coordinates for the current active UV map layer";
  ntype.enum_name_legacy = "INPUT_MESH_TEXTURE_COORDINATE";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_mesh_texture_coordinate_cc
