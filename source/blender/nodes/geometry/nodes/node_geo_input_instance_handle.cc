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
  // b.add_output<decl::String>("Name").field_source();
}

class InstanceBoundsField final : public bke::InstancesFieldInput {
 private:
  bool return_max_;

 public:
  InstanceBoundsField(bool return_max)
      : bke::InstancesFieldInput(CPPType::get<int>(), return_max ? "Max" : "Min"),
        return_max_(return_max)
  {
  }

  GVArray get_varray_for_context(const bke::Instances &instances,
                                 const IndexMask &mask) const final
  {
    const Span<int> handles = instances.reference_handles();
    // const Span<bke::InstanceReference> references = instances.references();
    // const Span<StringRef> instance_name = bke::Instances::instances.name();

    return VArray<int>::from_container(std::move(handles));
  }

  uint64_t hash() const override
  {
    return get_default_hash(return_max_);
  }

  bool is_equal_to(const fn::FieldNode &other) const override
  {
    if (const auto *other_field = dynamic_cast<const InstanceBoundsField *>(&other)) {
      return return_max_ == other_field->return_max_;
    }
    return false;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  params.set_output("Handle ID", Field<int>(std::make_shared<InstanceBoundsField>(false)));
  // params.set_output("Name", Field<StringRef>(std::make_shared<InstanceBoundsField>(true)));
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
