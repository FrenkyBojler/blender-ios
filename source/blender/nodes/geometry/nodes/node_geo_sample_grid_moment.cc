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

namespace blender::nodes::node_geo_sample_grid_moment_cc {

enum class MomentType {
  /* Scalar to vector. */
  ScalarFirst = 0,
  /* Scalar to matrix. */
  ScalarSecond = 1,
  /* Vector to matrix. */
  VectorFirst = 2,
};

enum class InterpolationMode {
  TriLinear = 0,
  QuadraticBSpline = 1,
  CubicBSpline = 2,
};

static const EnumPropertyItem moment_type_items[] = {
    {int(MomentType::ScalarFirst),
     "SCALAR_FIRST",
     0,
     N_("Scalar First"),
     "First moment of scalar, outputs a vector"},
    {int(MomentType::ScalarSecond),
     "SCALAR_SECOND",
     0,
     N_("Scalar Second"),
     "Second moment of scalar, outputs a matrix"},
    {int(MomentType::VectorFirst),
     "VECTOR_FIRST",
     0,
     N_("Vector First"),
     "First moment of vector, outputs a matrix"},
    {0, nullptr, 0, nullptr, nullptr},
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

static eNodeSocketDatatype get_input_type(const MomentType moment_type)
{
  switch (moment_type) {
    case MomentType::ScalarFirst:
      return SOCK_FLOAT;
    case MomentType::ScalarSecond:
      return SOCK_FLOAT;
    case MomentType::VectorFirst:
      return SOCK_VECTOR;
  }
  BLI_assert_unreachable();
  return SOCK_FLOAT;
}

static eNodeSocketDatatype get_output_type(const MomentType moment_type)
{
  switch (moment_type) {
    case MomentType::ScalarFirst:
      return SOCK_VECTOR;
    case MomentType::ScalarSecond:
      return SOCK_MATRIX;
    case MomentType::VectorFirst:
      return SOCK_MATRIX;
  }
  BLI_assert_unreachable();
  return SOCK_FLOAT;
}

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();
  if (!node) {
    return;
  }
  const MomentType moment_type = MomentType(node->custom1);
  const eNodeSocketDatatype input_type = get_input_type(moment_type);
  const eNodeSocketDatatype output_type = get_output_type(moment_type);

  b.add_input(input_type, "Grid"_ustr).hide_value().structure_type(StructureType::Grid);
  b.add_input<decl::Vector>("Position"_ustr).implicit_field(NODE_DEFAULT_INPUT_POSITION_FIELD);
  b.add_input<decl::Menu>("Interpolation"_ustr)
      .static_items(interpolation_mode_items)
      .default_value(InterpolationMode::TriLinear)
      .optional_label()
      .description("How to interpolate the values between neighboring voxels");

  b.add_output(output_type, "Moment"_ustr).dependent_field({1});
}

static std::optional<MomentType> moment_type_for_input_type(const bNodeSocket &socket)
{
  switch (socket.type) {
    case SOCK_FLOAT:
    case SOCK_BOOLEAN:
    case SOCK_INT:
      return MomentType::ScalarFirst;
    case SOCK_VECTOR:
    case SOCK_RGBA:
      return MomentType::VectorFirst;
    default:
      return std::nullopt;
  }
}

static std::optional<MomentType> moment_type_for_output_type(const bNodeSocket &socket)
{
  switch (socket.type) {
    case SOCK_VECTOR:
    case SOCK_RGBA:
      return MomentType::ScalarFirst;
    case SOCK_MATRIX:
      /* Ambiguous, could also be VectorFirst. */
      return MomentType::ScalarSecond;
    default:
      return std::nullopt;
  }
}

static void node_gather_link_search_ops(GatherLinkSearchOpParams &params)
{
  if (params.in_out() == SOCK_IN) {
    const std::optional<MomentType> moment_type = moment_type_for_input_type(
        params.other_socket());
    if (moment_type) {
      params.add_item(IFACE_("Grid"), [moment_type](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeSampleGridMoment");
        node.custom1 = int(*moment_type);
        params.update_and_connect_available_socket(node, "Grid"_ustr);
      });
    }
    const eNodeSocketDatatype other_type = eNodeSocketDatatype(params.other_socket().type);
    if (params.node_tree().typeinfo->validate_link(other_type, SOCK_VECTOR)) {
      params.add_item(IFACE_("Position"), [](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeSampleGridMoment");
        params.update_and_connect_available_socket(node, "Position"_ustr);
      });
    }
  }
  else {
    if (const std::optional<MomentType> moment_type = moment_type_for_output_type(
            params.other_socket()))
    {
      params.add_item(IFACE_("Moment"), [moment_type](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeSampleGridMoment");
        node.custom1 = int(*moment_type);
        params.update_and_connect_available_socket(node, "Moment"_ustr);
      });
    }
  }
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "moment_type", UI_ITEM_NONE, "", ICON_NONE);
}

#ifdef WITH_OPENVDB

template<int Moment, typename GridValueT, typename AttributeT>
void sample_grid(const bke::OpenvdbGridType<GridValueT> &grid,
                 const InterpolationMode interpolation,
                 const Span<float3> positions,
                 const IndexMask &mask,
                 GMutableSpan dst)
{
  using GridType = bke::OpenvdbGridType<GridValueT>;
  using AccessorT = typename GridType::ConstUnsafeAccessor;

  AccessorT accessor = grid.getConstUnsafeAccessor();

  auto sample_data = [&]<typename Sampler>() {
    MutableSpan<AttributeT> dst_typed = dst.typed<AttributeT>();
    mask.foreach_index([&](const int64_t i) {
      const float3 &pos = positions[i];
      const openvdb::Vec3R world_pos(pos.x, pos.y, pos.z);
      const openvdb::Vec3R index_pos = grid.transform().worldToIndex(world_pos);
      /* Special case: 2nd moments are always float3x3, but stored in float4x4. */
      if constexpr (std::is_same_v<AttributeT, float4x4>) {
        using AttributeTraits = bke::VolumeGridTraits<float3x3>;
        openvdb::Mat3s value;
        Sampler::template sample_moment<Moment>(accessor, grid.transform(), index_pos, value);
        dst_typed[i] = float4x4(AttributeTraits::to_blender(value));
      }
      else {
        using AttributeTraits = bke::VolumeGridTraits<AttributeT>;
        using PrimitiveT = typename AttributeTraits::PrimitiveType;
        PrimitiveT value;
        Sampler::template sample_moment<Moment>(accessor, grid.transform(), index_pos, value);
        dst_typed[i] = AttributeTraits::to_blender(value);
      }
    });
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
  MomentType moment_type_;
  InterpolationMode interpolation_;
  mf::Signature signature_;
  VolumeGridType grid_type_;
  /** Avoid accessing grid in #call function to avoid overhead for each multi-function call. */
  bke::VolumeTreeAccessToken tree_token_;
  const openvdb::GridBase *grid_base_ = nullptr;

 public:
  SampleGridFunction(bke::GVolumeGrid grid,
                     MomentType moment_type,
                     InterpolationMode interpolation)
      : grid_(std::move(grid)), moment_type_(moment_type), interpolation_(interpolation)
  {
    BLI_assert(grid_);

    const eNodeSocketDatatype output_type = get_output_type(moment_type);
    const CPPType *cpp_type = bke::socket_type_to_geo_nodes_base_cpp_type(output_type);
    mf::SignatureBuilder builder{"Sample Grid", signature_};
    builder.single_input<float3>("Position");
    builder.single_output("Moment", *cpp_type);
    this->set_signature(&signature_);

    grid_base_ = &grid_->grid(tree_token_);
    grid_type_ = grid_->grid_type();
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArraySpan<float3> positions = params.readonly_single_input<float3>(0, "Position");
    GMutableSpan dst = params.uninitialized_single_output(1, "Moment");

    switch (moment_type_) {
      case MomentType::ScalarFirst: {
        using GridType = bke::OpenvdbGridType<float>;
        sample_grid<1, float, float3>(
            static_cast<const GridType &>(*grid_base_), interpolation_, positions, mask, dst);
        break;
      }
      case MomentType::ScalarSecond: {
        using GridType = bke::OpenvdbGridType<float>;
        sample_grid<2, float, float4x4>(
            static_cast<const GridType &>(*grid_base_), interpolation_, positions, mask, dst);
        break;
      }
      case MomentType::VectorFirst: {
        using GridType = bke::OpenvdbGridType<float3>;
        sample_grid<1, float3, float4x4>(
            static_cast<const GridType &>(*grid_base_), interpolation_, positions, mask, dst);
        break;
      }
    }
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

  const MomentType moment_type = MomentType(params.node().custom1);
  const auto interpolation = params.get_input<InterpolationMode>("Interpolation"_ustr);
  bke::SocketValueVariant position = params.extract_input<bke::SocketValueVariant>(
      "Position"_ustr);

  std::string error_message;
  bke::SocketValueVariant output_value;
  if (!execute_multi_function_on_value_variant(
          std::make_shared<SampleGridFunction>(std::move(grid), moment_type, interpolation),
          {&position},
          {&output_value},
          params.user_data(),
          error_message))
  {
    params.set_default_remaining_outputs();
    params.error_message_add(NodeWarningType::Error, std::move(error_message));
    return;
  }

  params.set_output("Moment"_ustr, std::move(output_value));
#else
  node_geo_exec_with_missing_openvdb(params);
#endif
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = int(MomentType::ScalarFirst);
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(srna,
                    "moment_type",
                    "Moment Type",
                    "Type of moment to compute",
                    moment_type_items,
                    NOD_inline_enum_accessors(custom1),
                    int(MomentType::ScalarFirst));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeSampleGridMoment");
  ntype.ui_name = "Sample Grid Moment";
  ntype.ui_description =
      "Retrieve the first or second moment of values from the specified volume grid";
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

}  // namespace blender::nodes::node_geo_sample_grid_moment_cc
