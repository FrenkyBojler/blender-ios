/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_geometry_set_instances.hh"
#include "BKE_instances.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_instance_handle_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Int>("Handle ID").field_source();
}

class InstanceHandleFieldInput final : public bke::InstancesFieldInput {
 public:
  InstanceHandleFieldInput()
      : bke::InstancesFieldInput(CPPType::get<int>(), "Handle")
  {
  }
  GVArray get_varray_for_context(const bke::Instances &instances,
                                 const IndexMask & /*mask*/) const final
  {
    const Span<int> handles = instances.reference_handles();
    return VArray<int>::from_container(std::move(handles));
  }

  uint64_t hash() const override
  {
    return 32374372;
  }

  bool is_equal_to(const fn::FieldNode &other) const override
  {
    return dynamic_cast<const InstanceHandleFieldInput *>(&other) != nullptr;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  params.set_output("Handle ID", Field<int>(std::make_shared<InstanceHandleFieldInput>()));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeInputInstanceHandle");
  ntype.ui_name = "Instance Handle";
  ntype.ui_description = "Output the handle ID of the instance's geometry set";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_instance_handle_cc
