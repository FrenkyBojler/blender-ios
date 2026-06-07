/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_geometry_fields.hh"

#include "NOD_rna_define.hh"
#include "NOD_socket_search_link.hh"

#include "RNA_access.hh"
#include "RNA_enum_types.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_index_of_id_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Int>("ID"_ustr).default_input_type(NODE_DEFAULT_INPUT_ID_INDEX_FIELD);
  b.add_input<decl::Int>("Sample ID"_ustr).structure_type(StructureType::Field);

  b.add_output<decl::Int>("Index"_ustr)
      .propagate_references()
      .description("First index of sample ID");
  b.add_output<decl::Bool>("Is Valid"_ustr)
      .propagate_references()
      .description("Sample ID is exists");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "domain", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = int(AttrDomain::Point);
}

class IndexOfIDFieldInput final : public bke::GeometryFieldInput {
 private:
  Field<int> id_field_;
  Field<int> sample_id_field_;

 public:
  IndexOfIDFieldInput(Field<int> id_field,
                      Field<int> sample_id_field)
      : bke::GeometryFieldInput(CPPType::get<int>(), "Index of ID"),
        id_field_(std::move(id_field)),
        sample_id_field_(std::move(sample_id_field))
  {
  }

  GVArray get_varray_for_context(const bke::GeometryFieldContext &context,
                                 const IndexMask &mask) const final
  {
    const AttributeAccessor attributes = *context.attributes();
    const int64_t domain_size = attributes.domain_size(context.domain());
    fn::FieldEvaluator evaluator{context, domain_size};
    evaluator.add(id_field_);
    evaluator.add(sample_id_field_);
    evaluator.evaluate();
    const VArray<int> id_varray = evaluator.get_evaluated<int>(0);
    const VArray<int> sample_id_varray = evaluator.get_evaluated<int>(1);
    
    if (id_varray.is_single()) {
      const int id_value = id_varray.get_internal_single();
      if (sample_id_varray.is_single()) {
        const int sample_id_value = sample_id_varray.get_internal_single();
        return VArray<int>::from_single(id_value == sample_id_value ? 0 : -1, domain_size);
      }
      
      const VArraySpan sample_id_span(sample_id_varray);
      IndexMaskMemory memory;
      const IndexMask invalid_indices = IndexMask::from_predicate(mask, memory, [&](const int i) {
        return sample_id_span[i] != id_value;
      }, exec_mode::parallel);
      
      if (invalid_indices.is_empty()) {
        return VArray<int>::from_single(0, domain_size);
      }
      
      if (invalid_indices.size() == mask.size()) {
        return VArray<int>::from_single(-1, domain_size);
      }
      
      Array<int> indices(mask.min_array_size(), 0);
      index_mask::masked_fill(indices.as_mutable_span(), -1, invalid_indices);
      return VArray<int>::from_container(std::move(indices));
    }
    
    if (sample_id_varray.is_single()) {
      const int sample_id_value = sample_id_varray.get_internal_single();

      const VArraySpan id_span(id_varray);
      const int index = id_span.first_index_try(sample_id_value);
      return VArray<int>::from_single(index, domain_size);
    }

    Map<int, int> id_map;
    const VArraySpan<int> id_span(id_varray);
    for (const int i : IndexRange(domain_size)) {
      id_map.add(id_span[i], i);
    }

    Array<int> indices(mask.min_array_size());
    const VArraySpan<int> sample_id_span(sample_id_varray);
    mask.foreach_index([&](const int i) {
      indices[i] = id_map.lookup_default(sample_id_span[i], -1);
    }, exec_mode::parallel);

    return VArray<int>::from_container(std::move(indices));
  }

  void foreach_recursive_field(FunctionRef<void(const GField &)> fn) const override
  {
    fn(id_field_);
    fn(sample_id_field_);
  }

  void hash_unique(UniqueHashBytes &hash, fn::FieldHashDeep &deep_hash_cache) const override
  {
    static constexpr int8_t id = 0;
    hash.add(&id);
    hash.add(deep_hash_cache.ensure(id_field_));
    hash.add(deep_hash_cache.ensure(sample_id_field_));
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  Field<int> id_field = params.extract_input<Field<int>>("ID"_ustr);
  Field<int> sample_id_field = params.extract_input<Field<int>>("Sample ID"_ustr);
  const Field<int> index_field = Field<int>::from_input<IndexOfIDFieldInput>(std::move(id_field), std::move(sample_id_field));

  static auto is_valid_fn = mf::build::SI1_SO<int, bool>(
      "Is Valid Index",
      [](const int index) { return index != -1; },
      mf::build::exec_presets::SomeSpanOrSingle<1>());

  auto is_valid_op = FieldOperation::from(is_valid_fn, {index_field});
  const Field<bool> is_valid_field = Field<bool>(std::move(is_valid_op), 0);
  
  params.set_output("Index"_ustr, index_field);
  params.set_output("Is Valid"_ustr, is_valid_field);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeIndexOfID"_ustr);
  ntype.ui_name = "Index of ID";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_index_of_id_cc
