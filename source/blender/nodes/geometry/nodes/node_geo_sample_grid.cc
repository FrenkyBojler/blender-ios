/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_userdef_types.h"

#include "BKE_type_conversions.hh"
#include "BKE_volume_grid.hh"
#include "BKE_volume_openvdb.hh"

#include "NOD_rna_define.hh"
#include "NOD_socket_search_link.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_enum_types.hh"

#ifdef WITH_OPENVDB
#  include <openvdb/tools/Interpolation.h>
#endif

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_sample_grid_cc {

/** Grid sampler using a quadratic B-spline basis function as described in
 * Steffen et al., "Analysis and reduction of quadrature errors in the material point method (MPM)"
 *
 * The kernel is a piece-wise quadratic spline with two parts:
 * f(x) = -|x|^2 + 3/4               for   0 <= |x| < 1/2
 * f(x) = 1/2*|x|^2 - 3/2*|x| + 9/8  for 1/2 <= |x| < 3/2
 * f(x) = 0                          for 3/2 <= |x|
 *
 * This kernel has a range of 1.5 voxels. For sampling in the index space of i <= x <= i+1
 * the contribution of points [i-1, i, i+1, i+2] must be considered.
 * Shifting the kernel function to these voxel locations yields these contributions:
 * v(x) = v[i-1]*f(x+1) +   v[i]*f(x) + v[i+1]*f(x-1) + v[i+2]*f(x-2)
 *      =      A*f(x+1) +      B*f(x) +      C*f(x-1) +      D*f(x-2)
 *
 * This results in the following expressions for sampling in one dimension:
 * For 0 <= x < 1/2:
 *   v(x) = x^2*(1/2*A - B + 1/2*C) + x*(-1/2*A       + 1/2*C) + (1/8*A + 6/8*B + 1/8*C)
 * For 1/2 <= x < 1:
 *   v(x) = x^2*(1/2*B - C + 1/2*D) + x*(-3/2*B + 2*C - 1/2*D) + (9/8*B - 2/8*C + 1/8*D)
 */
struct QuadraticBSplineSampler {
  static const char *name()
  {
    return "quadratic_bspline";
  }

  template<class ValueT> static ValueT interpolate(const ValueT *value, double weight)
  {
    OPENVDB_NO_TYPE_CONVERSION_WARNING_BEGIN
    if (weight < 0.5) {
      const ValueT sqr = static_cast<ValueT>(0.5 * (value[0] + value[2]) - value[1]);
      const ValueT lin = static_cast<ValueT>(0.5 * (value[2] - value[0]));
      const ValueT con = static_cast<ValueT>(0.125 * (value[0] + value[2]) + 0.75 * value[1]);
      const auto temp = weight * (weight * sqr + lin) + con;
      return static_cast<ValueT>(temp);
    }

    const ValueT sqr = static_cast<ValueT>(0.5 * (value[1] + value[3]) - value[2]);
    const ValueT lin = static_cast<ValueT>(-0.75 * value[1] + 2 * value[2] - 0.5 * value[3]);
    const ValueT con = static_cast<ValueT>(1.125 * value[1] - 0.25 * value[2] + 0.125 * value[3]);
    const auto temp = weight * (weight * sqr + lin) + con;
    return static_cast<ValueT>(temp);
    OPENVDB_NO_TYPE_CONVERSION_WARNING_END
  }

  template<class ValueT, size_t N>
  static ValueT interpolate_3d(ValueT (&data)[N][N][N], const openvdb::Vec3R &uvw)
  {
    ValueT vx[4];
    for (int dx = 0; dx < 4; ++dx) {
      ValueT vy[4];
      for (int dy = 0; dy < 4; ++dy) {
        const ValueT *vz = &data[dx][dy][0];
        vy[dy] = interpolate(vz, uvw.z());
      }
      vx[dx] = interpolate(vy, uvw.y());
    }
    return interpolate(vx, uvw.x());
  }

  template<class TreeT>
  static bool sample(const TreeT &inTree,
                     const openvdb::Vec3R &inCoord,
                     typename TreeT::ValueType &result)
  {
    using ValueT = typename TreeT::ValueType;

    const openvdb::Vec3i inIdx = openvdb::tools::local_util::floorVec3(inCoord),
                         inLoIdx = inIdx - openvdb::Vec3i(1, 1, 1);
    const openvdb::Vec3R uvw = inCoord - inIdx;

    /* Retrieve the values of the 64 voxels surrounding the fractional source coordinates. */
    bool active = false;
    ValueT data[4][4][4];
    for (int dx = 0, ix = inLoIdx.x(); dx < 4; ++dx, ++ix) {
      for (int dy = 0, iy = inLoIdx.y(); dy < 4; ++dy, ++iy) {
        for (int dz = 0, iz = inLoIdx.z(); dz < 4; ++dz, ++iz) {
          if (inTree.probeValue(openvdb::Coord(ix, iy, iz), data[dx][dy][dz])) {
            active = true;
          }
        }
      }
    }

    result = interpolate_3d(data, uvw);

    return active;
  }

  template<class TreeT>
  static typename TreeT::ValueType sample(const TreeT &inTree, const openvdb::Vec3R &inCoord)
  {
    using ValueT = typename TreeT::ValueType;

    const openvdb::Vec3i inIdx = openvdb::tools::local_util::floorVec3(inCoord),
                         inLoIdx = inIdx - openvdb::Vec3i(1, 1, 1);
    const openvdb::Vec3R uvw = inCoord - inIdx;

    /* Retrieve the values of the 64 voxels surrounding the fractional source coordinates. */
    ValueT data[4][4][4];
    for (int dx = 0, ix = inLoIdx.x(); dx < 4; ++dx, ++ix) {
      for (int dy = 0, iy = inLoIdx.y(); dy < 4; ++dy, ++iy) {
        for (int dz = 0, iz = inLoIdx.z(); dz < 4; ++dz, ++iz) {
          inTree.getValue(openvdb::Coord(ix, iy, iz));
        }
      }
    }

    return interpolate_3d(data, uvw);
  }
};

/** Grid sampler for the derivative of the quadratic B-spline basis function described in
 * Steffen et al., "Analysis and reduction of quadrature errors in the material point method (MPM)"
 *
 * The kernel is a piece-wise linear function with two parts:
 * f(x) = -2*|x|                     for   0 <= |x| < 1/2
 * f(x) = |x| - 3/2                  for 1/2 <= |x| < 3/2
 * f(x) = 0                          for 3/2 <= |x|
 *
 * This kernel has a range of 1.5 voxels. For sampling in the index space of i <= x <= i+1
 * the contribution of points [i-1, i, i+1, i+2] must be considered.
 * Shifting the kernel function to these voxel locations yields these contributions:
 * v(x) = v[i-1]*f(x+1) +   v[i]*f(x) + v[i+1]*f(x-1) + v[i+2]*f(x-2)
 *      =      A*f(x+1) +      B*f(x) +      C*f(x-1) +      D*f(x-2)
 *
 * This results in the following expressions for sampling in one dimension:
 * For 0 <= x < 1/2:
 *   v(x) = x*(A - 2*B + C) + (-1/2*A       - 5/2*C)
 * For 1/2 <= x < 1:
 *   v(x) = x*(B - 2*C + D) + (-3/2*B - 2*C + 7/2*D)
 */
struct QuadraticBSplineGradientSampler {
  static const char *name()
  {
    return "quadratic_bspline_gradient";
  }

  template<class ValueT> static ValueT interpolate(const ValueT *value, double weight)
  {
    OPENVDB_NO_TYPE_CONVERSION_WARNING_BEGIN
    if (weight < 0.5) {
      const ValueT lin = static_cast<ValueT>(value[0] - 2.0 * value[1] + value[2]);
      const ValueT con = static_cast<ValueT>(-0.5 * value[0] - 2.5 * value[2]);
      const auto temp = weight * lin + con;
      return static_cast<ValueT>(temp);
    }

    const ValueT lin = static_cast<ValueT>(value[1] - 2.0 * value[2] + value[3]);
    const ValueT con = static_cast<ValueT>(-1.5 * value[1] - 2.0 * value[2] - 3.5 * value[3]);
    const auto temp = weight * lin + con;
    return static_cast<ValueT>(temp);
    OPENVDB_NO_TYPE_CONVERSION_WARNING_END
  }

  template<class ValueT, size_t N>
  static ValueT interpolate_3d(ValueT (&data)[N][N][N], const openvdb::Vec3R &uvw)
  {
    ValueT vx[4];
    for (int dx = 0; dx < 4; ++dx) {
      ValueT vy[4];
      for (int dy = 0; dy < 4; ++dy) {
        const ValueT *vz = &data[dx][dy][0];
        vy[dy] = interpolate(vz, uvw.z());
      }
      vx[dx] = interpolate(vy, uvw.y());
    }
    return interpolate(vx, uvw.x());
  }

  template<class TreeT>
  static bool sample(const TreeT &inTree,
                     const openvdb::Vec3R &inCoord,
                     typename TreeT::ValueType &result)
  {
    using ValueT = typename TreeT::ValueType;

    const openvdb::Vec3i inIdx = openvdb::tools::local_util::floorVec3(inCoord),
                         inLoIdx = inIdx - openvdb::Vec3i(1, 1, 1);
    const openvdb::Vec3R uvw = inCoord - inIdx;

    /* Retrieve the values of the 64 voxels surrounding the fractional source coordinates. */
    bool active = false;
    ValueT data[4][4][4];
    for (int dx = 0, ix = inLoIdx.x(); dx < 4; ++dx, ++ix) {
      for (int dy = 0, iy = inLoIdx.y(); dy < 4; ++dy, ++iy) {
        for (int dz = 0, iz = inLoIdx.z(); dz < 4; ++dz, ++iz) {
          if (inTree.probeValue(openvdb::Coord(ix, iy, iz), data[dx][dy][dz])) {
            active = true;
          }
        }
      }
    }

    result = interpolate_3d(data, uvw);

    return active;
  }

  template<class TreeT>
  static typename TreeT::ValueType sample(const TreeT &inTree, const openvdb::Vec3R &inCoord)
  {
    using ValueT = typename TreeT::ValueType;

    const openvdb::Vec3i inIdx = openvdb::tools::local_util::floorVec3(inCoord),
                         inLoIdx = inIdx - openvdb::Vec3i(1, 1, 1);
    const openvdb::Vec3R uvw = inCoord - inIdx;

    /* Retrieve the values of the 64 voxels surrounding the fractional source coordinates. */
    ValueT data[4][4][4];
    for (int dx = 0, ix = inLoIdx.x(); dx < 4; ++dx, ++ix) {
      for (int dy = 0, iy = inLoIdx.y(); dy < 4; ++dy, ++iy) {
        for (int dz = 0, iz = inLoIdx.z(); dz < 4; ++dz, ++iz) {
          inTree.getValue(openvdb::Coord(ix, iy, iz));
        }
      }
    }

    return interpolate_3d(data, uvw);
  }
};

enum class InterpolationMode {
  Nearest = 0,
  TriLinear = 1,
  TriQuadratic = 2,
  QuadraticBSpline = 3,
  QuadraticBSplineGradient = 4,
  CubicBSpline = 5,
  CubicBSplineGradient = 6,
};

static const EnumPropertyItem interpolation_mode_items[] = {
    {int(InterpolationMode::Nearest), "NEAREST", 0, N_("Nearest Neighbor"), ""},
    {int(InterpolationMode::TriLinear), "TRILINEAR", 0, N_("Trilinear"), ""},
    {int(InterpolationMode::TriQuadratic), "TRIQUADRATIC", 0, N_("Triquadratic"), ""},
    {int(InterpolationMode::QuadraticBSpline),
     "QUADRATIC_BSPLINE",
     0,
     N_("Quadratic B-Spline"),
     ""},
    {int(InterpolationMode::QuadraticBSplineGradient),
     "QUADRATIC_BSPLINE_GRADIENT",
     0,
     N_("Quadratic B-Spline Gradient"),
     ""},
    {int(InterpolationMode::CubicBSpline), "CUBIC_BSPLINE", 0, N_("Cubic B-Spline"), ""},
    {int(InterpolationMode::CubicBSplineGradient),
     "CUBIC_BSPLINE_GRADIENT",
     0,
     N_("Cubic B-Spline Gradient"),
     ""},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();
  if (!node) {
    return;
  }
  const eNodeSocketDatatype data_type = eNodeSocketDatatype(node->custom1);

  b.add_input(data_type, "Grid").hide_value().structure_type(StructureType::Grid);
  b.add_input<decl::Vector>("Position").implicit_field(NODE_DEFAULT_INPUT_POSITION_FIELD);
  b.add_input<decl::Menu>("Interpolation")
      .static_items(interpolation_mode_items)
      .default_value(InterpolationMode::TriLinear)
      .optional_label()
      .description("How to interpolate the values between neighboring voxels");

  b.add_output(data_type, "Value").dependent_field({1});
}

static std::optional<eNodeSocketDatatype> node_type_for_socket_type(const bNodeSocket &socket)
{
  switch (socket.type) {
    case SOCK_FLOAT:
      return SOCK_FLOAT;
    case SOCK_BOOLEAN:
      return SOCK_BOOLEAN;
    case SOCK_INT:
      return SOCK_INT;
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
    params.add_item(IFACE_("Grid"), [node_type](LinkSearchOpParams &params) {
      bNode &node = params.add_node("GeometryNodeSampleGrid");
      node.custom1 = *node_type;
      params.update_and_connect_available_socket(node, "Grid");
    });
    const eNodeSocketDatatype other_type = eNodeSocketDatatype(params.other_socket().type);
    if (params.node_tree().typeinfo->validate_link(other_type, SOCK_VECTOR)) {
      params.add_item(IFACE_("Position"), [](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeSampleGrid");
        params.update_and_connect_available_socket(node, "Position");
      });
    }
  }
  else {
    params.add_item(IFACE_("Value"), [node_type](LinkSearchOpParams &params) {
      bNode &node = params.add_node("GeometryNodeSampleGrid");
      node.custom1 = *node_type;
      params.update_and_connect_available_socket(node, "Value");
    });
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
                 MutableSpan<T> dst)
{
  using GridType = bke::OpenvdbGridType<T>;
  using GridValueT = typename GridType::ValueType;
  using AccessorT = typename GridType::ConstUnsafeAccessor;
  using TraitsT = typename bke::VolumeGridTraits<T>;
  AccessorT accessor = grid.getConstUnsafeAccessor();

  auto sample_data = [&]<typename Sampler>() {
    mask.foreach_index([&](const int64_t i) {
      const float3 &pos = positions[i];
      const openvdb::Vec3R world_pos(pos.x, pos.y, pos.z);
      const openvdb::Vec3R index_pos = grid.transform().worldToIndex(world_pos);
      GridValueT value;
      Sampler::sample(accessor, index_pos, value);
      dst[i] = TraitsT::to_blender(value);
    });
  };

  /* Use to the Nearest Neighbor sampler for Bool grids (no interpolation). */
  InterpolationMode real_interpolation = interpolation;
  if constexpr (std::is_same_v<T, bool>) {
    real_interpolation = InterpolationMode::Nearest;
  }
  switch (real_interpolation) {
    case InterpolationMode::TriLinear: {
      sample_data.template operator()<openvdb::tools::BoxSampler>();
      break;
    }
    case InterpolationMode::TriQuadratic: {
      sample_data.template operator()<openvdb::tools::QuadraticSampler>();
      break;
    }
    case InterpolationMode::Nearest: {
      sample_data.template operator()<openvdb::tools::PointSampler>();
      break;
    }
    case InterpolationMode::QuadraticBSpline: {
      sample_data.template operator()<QuadraticBSplineSampler>();
      break;
    }
    case InterpolationMode::QuadraticBSplineGradient: {
      sample_data.template operator()<QuadraticBSplineGradientSampler>();
      break;
    }
    case InterpolationMode::CubicBSpline: {
      sample_data.template operator()<openvdb::tools::PointSampler>();
      break;
    }
    case InterpolationMode::CubicBSplineGradient: {
      sample_data.template operator()<openvdb::tools::PointSampler>();
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
    const CPPType *cpp_type = bke::socket_type_to_geo_nodes_base_cpp_type(*data_type);
    mf::SignatureBuilder builder{"Sample Grid", signature_};
    builder.single_input<float3>("Position");
    builder.single_output("Value", *cpp_type);
    this->set_signature(&signature_);

    grid_base_ = &grid_->grid(tree_token_);
    grid_type_ = grid_->grid_type();
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArraySpan<float3> positions = params.readonly_single_input<float3>(0, "Position");
    GMutableSpan dst = params.uninitialized_single_output(1, "Value");

    BKE_volume_grid_type_to_blender_value_type(grid_type_, [&]<typename T>() {
      if constexpr (is_same_any_v<T, bool, float, int, float3>) {
        sample_grid<T>(static_cast<const bke::OpenvdbGridType<T> &>(*grid_base_),
                       interpolation_,
                       positions,
                       mask,
                       dst.typed<T>());
      }
    });
  }
};

#endif /* WITH_OPENVDB */

static void node_geo_exec(GeoNodeExecParams params)
{
#ifdef WITH_OPENVDB
  bke::GVolumeGrid grid = params.extract_input<bke::GVolumeGrid>("Grid");
  if (!grid) {
    params.set_default_remaining_outputs();
    return;
  }

  const auto interpolation = params.get_input<InterpolationMode>("Interpolation");
  bke::SocketValueVariant position = params.extract_input<bke::SocketValueVariant>("Position");

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

  params.set_output("Value", std::move(output_value));
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
  RNA_def_node_enum(srna,
                    "data_type",
                    "Data Type",
                    "Node socket data type",
                    rna_enum_node_socket_data_type_items,
                    NOD_inline_enum_accessors(custom1),
                    SOCK_FLOAT,
                    grid_socket_type_items_filter_fn);
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeSampleGrid", GEO_NODE_SAMPLE_GRID);
  ntype.ui_name = "Sample Grid";
  ntype.ui_description = "Retrieve values from the specified volume grid";
  ntype.enum_name_legacy = "SAMPLE_GRID";
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

}  // namespace blender::nodes::node_geo_sample_grid_cc
