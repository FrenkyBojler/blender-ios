/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_geometry_set_instances.hh"
#include "BKE_instances.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_instance_name_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::String>("Name"_ustr).field_source();
}

class InstanceNameField final : public bke::InstancesFieldInput { //replace int hash with string when possible
 public:
  InstanceNameField() : bke::InstancesFieldInput(CPPType::get<int>(), "Name") {}

  GVArray get_varray_for_context(const bke::Instances &instances,
                                 const IndexMask &mask) const final
  {
    const Span<int> handles = instances.reference_handles();
    const Span<bke::InstanceReference> references = instances.references();

    IndexMaskMemory memory;
    IndexMask reference_mask(references.size());
    Array<bool> reference_in_mask(references.size(), false);

    mask.foreach_index(
        [&](const int i) {
          const int handle = handles[i];
          if (handle < reference_in_mask.size()) {
            reference_in_mask[handle] = true;
          }
        },
        exec_mode::grain_size(2048));

    reference_mask = IndexMask::from_bools(reference_in_mask.as_span(), memory);
    Array<std::string> reference_name(references.size());

    reference_mask.foreach_index(
        [&](const int reference_index) {
          const bke::InstanceReference &reference = references[reference_index];
          reference_name[reference_index] = reference.name();
        },
        exec_mode::grain_size(128));

    Array<std::string> output_name(mask.min_array_size());
    mask.foreach_index(
        [&](const int instance_index) {
          output_name[instance_index] = reference_name[handles[instance_index]];
        },
        exec_mode::grain_size(4096));

    return VArray<std::string>::from_container(std::move(output_name));
  }

  uint64_t hash() const override
  {
    return 42374372;
  }

  bool is_equal_to(const fn::FieldInput &other) const override
  {
    return dynamic_cast<const InstanceNameField *>(&other) != nullptr;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  params.set_output("Name"_ustr, Field<std::string>::from_input<InstanceNameField>());
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeInputInstanceName"_ustr);
  ntype.ui_name = "Instance Name";
  ntype.ui_description = "Output the name of each instance's geometry set";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_instance_name_cc
