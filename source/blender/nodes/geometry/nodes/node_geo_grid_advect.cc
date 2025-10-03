/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_volume_grid.hh"

#include "NOD_socket_search_link.hh"

#include "node_geometry_util.hh"

#ifdef WITH_OPENVDB
#  include "openvdb/tools/VolumeAdvect.h"
#endif

namespace blender::nodes::node_geo_grid_advect_cc {

enum class IntegrationScheme : int8_t {
  SemiLagrangian = 0,
  Midpoint = 1,
  RungeKutta3 = 2,
  RungeKutta4 = 3,
  MacCormack = 4,
  BFECC = 5,
};

static const EnumPropertyItem integration_scheme_items[] = {
    {int(IntegrationScheme::SemiLagrangian),
     "SEMI",
     0,
     "Semi-Lagrangian",
     "1st order semi-Lagrangian integration. Fast but least accurate, suitable for simple "
     "advection"},
    {int(IntegrationScheme::Midpoint),
     "MID",
     0,
     "Midpoint",
     "2nd order midpoint integration. Good balance between speed and accuracy for most cases"},
    {int(IntegrationScheme::RungeKutta3),
     "RK3",
     0,
     "Runge-Kutta 3",
     "3rd order Runge-Kutta integration. Higher accuracy at moderate computational cost"},
    {int(IntegrationScheme::RungeKutta4),
     "RK4",
     0,
     "Runge-Kutta 4",
     "4th order Runge-Kutta integration. Highest accuracy single-step method but slower"},
    {int(IntegrationScheme::MacCormack),
     "MAC",
     0,
     "MacCormack",
     "MacCormack scheme with implicit diffusion control. Reduces numerical dissipation while "
     "maintaining stability"},
    {int(IntegrationScheme::BFECC),
     "BFECC",
     0,
     "BFECC",
     "Back and Forth Error Compensation and Correction. Advanced scheme that minimizes "
     "dissipation and diffusion"},
    {0, nullptr, 0, nullptr, nullptr},
};

enum class LimiterType : int8_t {
  None = 0,
  Clamp = 1,
  Revert = 2,
};

static const EnumPropertyItem limiter_type_items[] = {
    {int(LimiterType::None),
     "NONE",
     0,
     "None",
     "No limiting applied. Fastest but may produce artifacts in high-order schemes"},
    {int(LimiterType::Clamp),
     "CLAMP",
     0,
     "Clamp",
     "Clamp values to the range of the original neighborhood. Prevents overshooting and "
     "undershooting"},
    {int(LimiterType::Revert),
     "REVERT",
     0,
     "Revert",
     "Revert to 1st order integration when clamping would be applied. More conservative than "
     "clamping"},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_default_layout();

  const bNode *node = b.node_or_null();
  if (!node) {
    return;
  }

  const eNodeSocketDatatype data_type = eNodeSocketDatatype(node->custom1);
  b.add_input(data_type, "Grid").hide_value().structure_type(StructureType::Grid);
  b.add_output(data_type, "Grid").structure_type(StructureType::Grid).align_with_previous();
  b.add_input<decl::Vector>("Velocity").hide_value().structure_type(StructureType::Grid);
  b.add_input<decl::Menu>("Integration Scheme")
      .static_items(integration_scheme_items)
      .default_value(IntegrationScheme::RungeKutta3)
      .optional_label()
      .description("Numerical integration method for advection");
  b.add_input<decl::Menu>("Limiter")
      .static_items(limiter_type_items)
      .default_value(LimiterType::Clamp)
      .optional_label()
      .description("Limiting strategy to prevent numerical artifacts");
  b.add_input<decl::Float>("Time Step")
      .default_value(1.0f)
      .min(0.0f)
      .description("Time step for advection in seconds");
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout->prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
}

static std::optional<eNodeSocketDatatype> node_type_for_socket_type(const bNodeSocket &socket)
{
  switch (socket.type) {
    case SOCK_FLOAT:
      return SOCK_FLOAT;
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
  if (!USER_EXPERIMENTAL_TEST(&U, use_new_volume_nodes)) {
    return;
  }
  const std::optional<eNodeSocketDatatype> data_type = node_type_for_socket_type(
      params.other_socket());
  if (!data_type) {
    return;
  }
  params.add_item(IFACE_("Grid"), [data_type](LinkSearchOpParams &params) {
    bNode &node = params.add_node("GeometryNodeGridAdvect");
    node.custom1 = *data_type;
    params.update_and_connect_available_socket(node, "Grid");
  });
}

#ifdef WITH_OPENVDB
template<typename GridType, typename SamplerType = openvdb::tools::Sampler<1>>
static typename GridType::Ptr advect_grid(const GridType &grid,
                                          const openvdb::Vec3SGrid &velocity_grid,
                                          const float time_step,
                                          const IntegrationScheme scheme,
                                          const LimiterType limiter)
{
  openvdb::tools::VolumeAdvection<openvdb::Vec3SGrid, false> advection(velocity_grid);

  /* Set integration scheme. */
  switch (scheme) {
    case IntegrationScheme::SemiLagrangian:
      advection.setIntegrator(openvdb::tools::Scheme::SEMI);
      break;
    case IntegrationScheme::Midpoint:
      advection.setIntegrator(openvdb::tools::Scheme::MID);
      break;
    case IntegrationScheme::RungeKutta3:
      advection.setIntegrator(openvdb::tools::Scheme::RK3);
      break;
    case IntegrationScheme::RungeKutta4:
      advection.setIntegrator(openvdb::tools::Scheme::RK4);
      break;
    case IntegrationScheme::MacCormack:
      advection.setIntegrator(openvdb::tools::Scheme::MAC);
      break;
    case IntegrationScheme::BFECC:
      advection.setIntegrator(openvdb::tools::Scheme::BFECC);
      break;
  }

  /* Set limiter. */
  switch (limiter) {
    case LimiterType::None:
      advection.setLimiter(openvdb::tools::Scheme::NO_LIMITER);
      break;
    case LimiterType::Clamp:
      advection.setLimiter(openvdb::tools::Scheme::CLAMP);
      break;
    case LimiterType::Revert:
      advection.setLimiter(openvdb::tools::Scheme::REVERT);
      break;
  }

  return advection.template advect<GridType, SamplerType>(grid, time_step);
}
#endif

static void node_geo_exec(GeoNodeExecParams params)
{
#ifdef WITH_OPENVDB
  bke::GVolumeGrid grid = params.extract_input<bke::GVolumeGrid>("Grid");
  if (!grid) {
    params.set_default_remaining_outputs();
    return;
  }

  const bke::VolumeGrid<float3> velocity_grid = params.extract_input<bke::VolumeGrid<float3>>(
      "Velocity");
  if (!velocity_grid) {
    params.set_default_remaining_outputs();
    return;
  }

  const float time_step = params.extract_input<float>("Time Step");
  const IntegrationScheme scheme = params.get_input<IntegrationScheme>("Integration Scheme");
  const LimiterType limiter = params.get_input<LimiterType>("Limiter");

  bke::VolumeTreeAccessToken tree_token;
  const openvdb::Vec3SGrid &velocity_vdb_grid = velocity_grid.grid(tree_token);

  const VolumeGridType grid_type = grid->grid_type();
  switch (grid_type) {
    case VOLUME_GRID_FLOAT: {
      const bke::VolumeGrid<float> typed_grid = grid.typed<float>();
      const openvdb::FloatGrid &vdb_grid = typed_grid.grid(tree_token);
      openvdb::FloatGrid::Ptr result = advect_grid(
          vdb_grid, velocity_vdb_grid, time_step, scheme, limiter);
      params.set_output("Grid", bke::GVolumeGrid(bke::VolumeGrid<float>(std::move(result))));
      break;
    }
    case VOLUME_GRID_INT: {
      const bke::VolumeGrid<int> typed_grid = grid.typed<int>();
      const openvdb::Int32Grid &vdb_grid = typed_grid.grid(tree_token);
      openvdb::Int32Grid::Ptr result = advect_grid(
          vdb_grid, velocity_vdb_grid, time_step, scheme, limiter);
      params.set_output("Grid", bke::GVolumeGrid(bke::VolumeGrid<int>(std::move(result))));
      break;
    }
    case VOLUME_GRID_VECTOR_FLOAT: {
      const bke::VolumeGrid<float3> typed_grid = grid.typed<float3>();
      const openvdb::Vec3fGrid &vdb_grid = typed_grid.grid(tree_token);
      openvdb::Vec3fGrid::Ptr result = advect_grid(
          vdb_grid, velocity_vdb_grid, time_step, scheme, limiter);
      params.set_output("Grid", bke::GVolumeGrid(bke::VolumeGrid<float3>(std::move(result))));
      break;
    }
    case VOLUME_GRID_BOOLEAN:
    case VOLUME_GRID_MASK:
    case VOLUME_GRID_UNKNOWN:
    case VOLUME_GRID_DOUBLE:
    case VOLUME_GRID_INT64:
    case VOLUME_GRID_VECTOR_DOUBLE:
    case VOLUME_GRID_VECTOR_INT:
    case VOLUME_GRID_POINTS:
      params.error_message_add(NodeWarningType::Error, "Unsupported grid type for advection");
      params.set_default_remaining_outputs();
      return;
  }
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
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeGridAdvect");
  ntype.ui_name = "Advect Grid";
  ntype.ui_description =
      "Move grid values through a velocity field using numerical integration. Supports multiple "
      "integration schemes for different accuracy and performance trade-offs";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.initfunc = node_init;
  ntype.gather_link_search_ops = node_gather_link_search_ops;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);
  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_grid_advect_cc
