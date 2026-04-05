/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_matrix_types.hh"

#include "BKE_volume_grid.hh"
#include "BKE_volume_openvdb.hh"

#include "GEO_grid_samplers.hh"

#include "NOD_rna_define.hh"
#include "NOD_socket_search_link.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_enum_types.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_sample_grid_gradient_cc {

enum class InterpolationMode {
  TriLinear = 0,
  QuadraticBSpline = 1,
  CubicBSpline = 2,
};

static const EnumPropertyItem interpolation_mode_items[] = {
    {int(InterpolationMode::TriLinear), "TRILINEAR", 0, N_("Trilinear"), ""},
    {int(InterpolationMode::QuadraticBSpline),
     "QUADRATIC_BSPLINE",
     0,
     N_("Quadratic B-Spline"),
     ""},
    {int(InterpolationMode::CubicBSpline), "CUBIC_BSPLINE", 0, N_("Cubic B-Spline"), ""},
    {0, nullptr, 0, nullptr, nullptr},
};

/* Returns the type of gradients for a given socket type, if possible. */
static std::optional<eNodeSocketDatatype> gradient_type_from_data_type(
    const eNodeSocketDatatype data_type)
{
  switch (data_type) {
    case SOCK_FLOAT:
      return SOCK_VECTOR;
    case SOCK_VECTOR:
      return SOCK_MATRIX;
    default:
      return std::nullopt;
  }
}

/* Returns the default data type used to create a gradient type. */
static std::optional<eNodeSocketDatatype> data_type_from_gradient_type(
    const eNodeSocketDatatype gradient_type)
{
  switch (gradient_type) {
    case SOCK_VECTOR:
      return SOCK_FLOAT;
    case SOCK_MATRIX:
      return SOCK_VECTOR;
    default:
      return std::nullopt;
  }
}

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();
  if (!node) {
    return;
  }
  const eNodeSocketDatatype data_type = eNodeSocketDatatype(node->custom1);

  b.add_input(data_type, "Grid"_ustr).hide_value().structure_type(StructureType::Grid);
  b.add_input<decl::Vector>("Position"_ustr).implicit_field(NODE_DEFAULT_INPUT_POSITION_FIELD);
  b.add_input<decl::Menu>("Interpolation"_ustr)
      .static_items(interpolation_mode_items)
      .default_value(InterpolationMode::TriLinear)
      .optional_label()
      .description("How to interpolate the values between neighboring voxels");

  if (const std::optional<eNodeSocketDatatype> gradient_type = gradient_type_from_data_type(
          data_type))
  {
    b.add_output(*gradient_type, "Gradient"_ustr).dependent_field({1});
  }
}

static std::optional<eNodeSocketDatatype> node_type_for_socket_type(const bNodeSocket &socket)
{
  switch (socket.type) {
    case SOCK_FLOAT:
    case SOCK_BOOLEAN:
    case SOCK_INT:
      return SOCK_FLOAT;
    case SOCK_VECTOR:
    case SOCK_RGBA:
      return SOCK_VECTOR;
    default:
      return std::nullopt;
  }
}

static void node_gather_link_search_ops(GatherLinkSearchOpParams &params)
{
  const std::optional<eNodeSocketDatatype> node_type = node_type_for_socket_type(
      params.other_socket());
  if (!node_type) {
    return;
  }
  if (params.in_out() == SOCK_IN) {
    if (gradient_type_from_data_type(*node_type)) {
      params.add_item(IFACE_("Grid"), [node_type](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeSampleGridGradient");
        node.custom1 = *node_type;
        params.update_and_connect_available_socket(node, "Grid"_ustr);
      });
    }
    const eNodeSocketDatatype other_type = eNodeSocketDatatype(params.other_socket().type);
    if (params.node_tree().typeinfo->validate_link(other_type, SOCK_VECTOR)) {
      params.add_item(IFACE_("Position"), [](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeSampleGridGradient");
        params.update_and_connect_available_socket(node, "Position"_ustr);
      });
    }
  }
  else {
    if (const std::optional<eNodeSocketDatatype> data_type = data_type_from_gradient_type(
            *node_type))
    {
      params.add_item(IFACE_("Gradient"), [data_type](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeSampleGridGradient");
        node.custom1 = *data_type;
        params.update_and_connect_available_socket(node, "Gradient"_ustr);
      });
    }
  }
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
}

#ifdef WITH_OPENVDB

template<typename T>
void sample_grid(const bke::OpenvdbGridType<T> &grid,
                 const InterpolationMode interpolation,
                 const Span<float3> positions,
                 const IndexMask &mask,
                 GMutableSpan dst)
{
  using GridType = bke::OpenvdbGridType<T>;
  using GridValueT = typename GridType::ValueType;
  using GridGradientT = geometry::grid_sampling::OpenvdbGradientType<GridValueT>;
  using GradientT = geometry::grid_sampling::GradientType<T>;
  using AccessorT = typename GridType::ConstUnsafeAccessor;
  using TraitsT = typename bke::VolumeGridTraits<GradientT>;
  AccessorT accessor = grid.getConstUnsafeAccessor();

  auto sample_data = [&]<typename Sampler>() {
    if constexpr (std::is_same_v<GradientT, float3x3>) {
      /* float3x3 needs to be converted to float4x4 field type. */
      MutableSpan<float4x4> dst_typed = dst.typed<float4x4>();
      mask.foreach_index([&](const int64_t i) {
        const float3 &pos = positions[i];
        const openvdb::Vec3R world_pos(pos.x, pos.y, pos.z);
        const openvdb::Vec3R index_pos = grid.transform().worldToIndex(world_pos);
        GridGradientT value;
        Sampler::sample_gradient(accessor, grid.transform(), index_pos, value);
        dst_typed[i] = float4x4(TraitsT::to_blender(value));
      });
    }
    else {
      MutableSpan<GradientT> dst_typed = dst.typed<GradientT>();
      mask.foreach_index([&](const int64_t i) {
        const float3 &pos = positions[i];
        const openvdb::Vec3R world_pos(pos.x, pos.y, pos.z);
        const openvdb::Vec3R index_pos = grid.transform().worldToIndex(world_pos);
        GridGradientT value;
        Sampler::sample_gradient(accessor, grid.transform(), index_pos, value);
        dst_typed[i] = TraitsT::to_blender(value);
      });
    }
  };

  /* Use to the Nearest Neighbor sampler for Bool grids (no interpolation). */
  InterpolationMode real_interpolation = interpolation;
  switch (real_interpolation) {
    case InterpolationMode::TriLinear: {
      sample_data.template operator()<geometry::LinearSampler>();
      break;
    }
    case InterpolationMode::QuadraticBSpline: {
      sample_data.template operator()<geometry::QuadraticBSplineSampler>();
      break;
    }
    case InterpolationMode::CubicBSpline: {
      sample_data.template operator()<geometry::CubicBSplineSampler>();
      break;
    }
  }
}

class SampleGridFunction : public mf::MultiFunction {
  bke::GVolumeGrid grid_;
  InterpolationMode interpolation_;
  mf::Signature signature_;
  VolumeGridType grid_type_;
  /** Avoid accessing grid in #call function to avoid overhead for each multi-function call. */
  bke::VolumeTreeAccessToken tree_token_;
  const openvdb::GridBase *grid_base_ = nullptr;

 public:
  SampleGridFunction(bke::GVolumeGrid grid, InterpolationMode interpolation)
      : grid_(std::move(grid)), interpolation_(interpolation)
  {
    BLI_assert(grid_);

    const std::optional<eNodeSocketDatatype> data_type = bke::grid_type_to_socket_type(
        grid_->grid_type());
    const std::optional<eNodeSocketDatatype> gradient_type = gradient_type_from_data_type(
        *data_type);
    const CPPType *cpp_type = bke::socket_type_to_geo_nodes_base_cpp_type(*gradient_type);
    mf::SignatureBuilder builder{"Sample Grid", signature_};
    builder.single_input<float3>("Position");
    builder.single_output("Gradient", *cpp_type);
    this->set_signature(&signature_);

    grid_base_ = &grid_->grid(tree_token_);
    grid_type_ = grid_->grid_type();
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArraySpan<float3> positions = params.readonly_single_input<float3>(0, "Position");
    GMutableSpan dst = params.uninitialized_single_output(1, "Gradient");

    BKE_volume_grid_type_to_blender_value_type(grid_type_, [&]<typename T>() {
      if constexpr (is_same_any_v<T, float, float3>) {
        sample_grid<T>(static_cast<const bke::OpenvdbGridType<T> &>(*grid_base_),
                       interpolation_,
                       positions,
                       mask,
                       dst);
      }
    });
  }
};

#endif /* WITH_OPENVDB */

static void node_geo_exec(GeoNodeExecParams params)
{
#ifdef WITH_OPENVDB
  bke::GVolumeGrid grid = params.extract_input<bke::GVolumeGrid>("Grid"_ustr);
  if (!grid) {
    params.set_default_remaining_outputs();
    return;
  }

  const auto interpolation = params.get_input<InterpolationMode>("Interpolation"_ustr);
  bke::SocketValueVariant position = params.extract_input<bke::SocketValueVariant>(
      "Position"_ustr);

  std::string error_message;
  bke::SocketValueVariant output_value;
  if (!execute_multi_function_on_value_variant(
          std::make_shared<SampleGridFunction>(std::move(grid), interpolation),
          {&position},
          {&output_value},
          params.user_data(),
          error_message))
  {
    params.set_default_remaining_outputs();
    params.error_message_add(NodeWarningType::Error, std::move(error_message));
    return;
  }

  params.set_output("Gradient"_ustr, std::move(output_value));
#else
  node_geo_exec_with_missing_openvdb(params);
#endif
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = SOCK_FLOAT;
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(
      srna,
      "data_type",
      "Data Type",
      "Node socket data type",
      rna_enum_node_socket_data_type_items,
      NOD_inline_enum_accessors(custom1),
      SOCK_FLOAT,
      [](bContext * /*C*/, PointerRNA * /*ptr*/, PropertyRNA * /*prop*/, bool *r_free)
          -> const EnumPropertyItem * {
        *r_free = true;
        return enum_items_filter(
            rna_enum_node_socket_data_type_items, [](const EnumPropertyItem &item) -> bool {
              return ELEM(eNodeSocketDatatype(item.value), SOCK_FLOAT, SOCK_VECTOR);
            });
      });
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeSampleGridGradient");
  ntype.ui_name = "Sample Grid Gradient";
  ntype.ui_description = "Retrieve the gradient of values from the specified volume grid";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  ntype.gather_link_search_ops = node_gather_link_search_ops;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_sample_grid_gradient_cc
