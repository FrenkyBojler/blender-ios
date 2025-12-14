/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute_math.hh"

#include "BLI_array.hh"
#include "BLI_generic_virtual_array.hh"
#include "BLI_virtual_array.hh"

#include "NOD_rna_define.hh"
#include "NOD_socket_search_link.hh"

#include "RNA_enum_types.hh"

#include "node_geometry_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include <numeric>

namespace blender::nodes::node_geo_field_any_all_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Bool>("Value")
      .supports_field();

  b.add_input<decl::Int>("Group ID", "Group Index")
      .supports_field()
      .hide_value()
      .description("An index used to group values together for multiple separate operations");

  b.add_output<decl::Bool>("Any")
      .field_source_reference_all()
      .description("True if at least one value is True");

  b.add_output<decl::Bool>("All")
      .field_source_reference_all()
      .description("True only if all values are True");
}

enum class Operation { Any = 0, All = 1 };

class AnyAllInput final : public bke::GeometryFieldInput {
 private:
  Field<bool> input_;
  Field<int> group_index_;
  Operation operation_;

 public:
  AnyAllInput(Field<bool> input, Field<int> group_index, Operation operation)
      : bke::GeometryFieldInput(CPPType::get<bool>(), "Calculation"),
        input_(std::move(input)),
        group_index_(std::move(group_index)),
        operation_(operation)
  {
  }

  GVArray get_varray_for_context(const bke::GeometryFieldContext &context,
                                 const IndexMask & /*mask*/) const final
  {
    const AttributeAccessor attributes = *context.attributes();
    const AttrDomain domain = context.domain();
    const int64_t domain_size = attributes.domain_size(domain);

    if (domain_size == 0) {
      return {};
    }

    fn::FieldEvaluator evaluator{context, domain_size};
    evaluator.add(input_);
    evaluator.add(group_index_);
    evaluator.evaluate();

    const GVArray g_values = evaluator.get_evaluated(0);
    const VArray<int> group_indices = evaluator.get_evaluated<int>(1);
    const VArray<bool> values = g_values.typed<bool>();

    GVArray g_outputs;

    if (operation_ == Operation::Any) {
      if (group_indices.is_single()) {
        bool any = false;
        for (const int i : values.index_range()) {
          if (values[i]) {
            any = true;
            break;
          }
        }
        return VArray<bool>::from_single(any, domain_size);
      }
      else {
        Array<bool> outputs(domain_size);
        Set<int> groups;

        for (const int i : values.index_range()) {
          if (values[i]) {
            groups.add(group_indices[i]);
          }
        }

        for (const int i : outputs.index_range()) {
          outputs[i] = groups.contains(group_indices[i]);
        }

        return VArray<bool>::from_container(std::move(outputs));
      }
    }
    else if (operation_ == Operation::All) {
      if (group_indices.is_single()) {
        bool all = true;
        for (const int i : values.index_range()) {
          if (!values[i]) {
            all = false;
            break;
          }
        }
        return VArray<bool>::from_single(all, domain_size);
      }
      else {
        Array<bool> outputs(domain_size);
        Set<int> groups;

        for (const int i : values.index_range()) {
          if (!values[i]) {
            groups.add(group_indices[i]);
          }
        }

        for (const int i : outputs.index_range()) {
          outputs[i] = !groups.contains(group_indices[i]);
        }

        return VArray<bool>::from_container(std::move(outputs));
      }
    }
  }

  void for_each_field_input_recursive(FunctionRef<void(const FieldInput &)> fn) const final
  {
    input_.node().for_each_field_input_recursive(fn);
    group_index_.node().for_each_field_input_recursive(fn);
  }

  uint64_t hash() const override
  {
    return get_default_hash(input_, group_index_, operation_);
  }

  bool is_equal_to(const fn::FieldNode &other) const override
  {
    if (const AnyAllInput *other_field = dynamic_cast<const AnyAllInput *>(&other)) {
      return input_ == other_field->input_ && group_index_ == other_field->group_index_ &&
             operation_ == other_field->operation_;
    }
    return false;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  const Field<int> group_index_field = params.extract_input<Field<int>>("Group Index");
  const Field<bool> input_field = params.extract_input<Field<bool>>("Value");

  if (params.output_is_required("Any")) {
    params.set_output("Any",
                      Field<bool>{std::make_shared<AnyAllInput>(
                          input_field, group_index_field, Operation::Any)});
  }

  if (params.output_is_required("All")) {
    params.set_output("All",
                      Field<bool>{std::make_shared<AnyAllInput>(
                          input_field, group_index_field, Operation::All)});
  }
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeFieldAnyAll");
  ntype.ui_name = "Field Any & All";
  ntype.ui_description = "Calculates if any/all values in a boolean field is True";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_field_any_all_cc
