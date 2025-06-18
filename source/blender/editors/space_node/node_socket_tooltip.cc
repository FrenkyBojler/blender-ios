/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <fmt/format.h>
#include <sstream>

#include "BKE_context.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_node_enum.hh"
#include "BKE_node_runtime.hh"
#include "BKE_type_conversions.hh"

#include "BLI_math_euler.hh"
#include "BLI_string.h"

#include "BLT_translation.hh"

#include "DNA_collection_types.h"
#include "DNA_material_types.h"

#include "NOD_geometry_nodes_log.hh"
#include "NOD_node_declaration.hh"

#include "node_intern.hh"

namespace geo_log = blender::nodes::geo_eval_log;

namespace blender::ed::space_node {

static void build_tooltip_label(uiTooltipData &tip_data, const bNodeSocket &socket)
{
  const bNode &node = socket.owner_node();
  if (node.is_reroute()) {
    UI_tooltip_text_field_add(tip_data, TIP_("Reroute"), {}, UI_TIP_STYLE_HEADER, UI_TIP_LC_MAIN);
    return;
  }
  const StringRefNull translated_socket_label = node_socket_get_label(&socket, nullptr);
  UI_tooltip_text_field_add(
      tip_data, translated_socket_label, {}, UI_TIP_STYLE_HEADER, UI_TIP_LC_MAIN);
}

static void add_space(uiTooltipData &tip_data, const int amount = 1)
{
  for ([[maybe_unused]] const int i : IndexRange(amount)) {
    UI_tooltip_text_field_add(tip_data, {}, {}, UI_TIP_STYLE_SPACER, UI_TIP_LC_NORMAL);
  }
}

static std::optional<std::string> get_socket_description(const bNodeSocket &socket)
{
  if (socket.runtime->declaration == nullptr) {
    if (socket.description[0]) {
      return socket.description;
    }
    return std::nullopt;
  }
  const blender::nodes::SocketDeclaration &socket_decl = *socket.runtime->declaration;
  blender::StringRefNull description = socket_decl.description;
  if (description.is_empty()) {
    return std::nullopt;
  }

  return TIP_(description);
}

static void build_tooltip_description(uiTooltipData &tip_data, const bNodeSocket &socket)
{
  std::optional<std::string> description_opt = get_socket_description(socket);
  if (!description_opt) {
    return;
  }
  std::string description = std::move(*description_opt);
  if (description.empty()) {
    return;
  }
  if (description[description.size() - 1] != '.') {
    description += '.';
  }
  add_space(tip_data, 2);
  UI_tooltip_text_field_add(
      tip_data, std::move(description), {}, UI_TIP_STYLE_NORMAL, UI_TIP_LC_NORMAL);
}

static void build_tooltip_value_and_type_oneline(uiTooltipData &tip_data,
                                                 const StringRef value,
                                                 const StringRef type)
{
  UI_tooltip_text_field_add(tip_data,
                            fmt::format("{}: {}", TIP_("Value"), value),
                            {},
                            UI_TIP_STYLE_MONO,
                            UI_TIP_LC_VALUE);
  add_space(tip_data);
  UI_tooltip_text_field_add(
      tip_data, fmt::format("{}: {}", TIP_("Type"), type), {}, UI_TIP_STYLE_MONO, UI_TIP_LC_VALUE);
}

template<typename T>
[[nodiscard]] static bool build_tooltip_value_data_block(uiTooltipData &tip_data,
                                                         const GPointer &value)
{
  const CPPType &type = *value.type();
  if (!type.is<T *>()) {
    return false;
  }
  const T *data = *value.get<T *>();
  std::string value_str;
  if (data) {
    value_str = BKE_id_name(id_cast<const ID &>(*data));
  }
  else {
    value_str = TIP_("None");
  }
  const ID_Type id_type = T::id_type;
  const char *id_type_name = BKE_idtype_idcode_to_name(id_type);

  build_tooltip_value_and_type_oneline(tip_data, value_str, TIP_(id_type_name));
  return true;
}

static void build_tooltip_value_enum(uiTooltipData &tip_data,
                                     const bNodeSocket &socket,
                                     const int item_identifier)
{
  const auto *storage = socket.default_value_typed<bNodeSocketValueMenu>();
  if (!storage->enum_items || storage->has_conflict()) {
    build_tooltip_value_and_type_oneline(tip_data, TIP_("Unknown"), TIP_("Menu"));
    return;
  }
  const bke::RuntimeNodeEnumItem *enum_item = storage->enum_items->find_item_by_identifier(
      item_identifier);
  if (!enum_item) {
    return;
  }
  if (!enum_item->description.empty()) {
    UI_tooltip_text_field_add(
        tip_data, enum_item->description, {}, UI_TIP_STYLE_NORMAL, UI_TIP_LC_VALUE);
    add_space(tip_data);
  }
  build_tooltip_value_and_type_oneline(tip_data, enum_item->name, TIP_("Menu"));
}

static void build_tooltip_value_int(uiTooltipData &tip_data, const int value)
{
  std::string value_str = fmt::format("{}", value);
  build_tooltip_value_and_type_oneline(tip_data, value_str, TIP_("Integer"));
}

static void build_tooltip_value_float(uiTooltipData &tip_data, const float value)
{
  std::string value_str;
  /* Above that threshold floats can't represent fractions anymore. */
  if (std::abs(value) > (1 << 24)) {
    /* Use higher precision to display correct integer value instead of one that is rounded to
     * fewer significant digits. */
    value_str = fmt::format("{:.10}", value);
  }
  else {
    value_str = fmt::format("{}", value);
  }
  build_tooltip_value_and_type_oneline(tip_data, value_str, TIP_("Float"));
}

static void build_tooltip_value_float3(uiTooltipData &tip_data, const float3 &value)
{
  const std::string value_str = fmt::format("{} {} {}", value.x, value.y, value.z);
  build_tooltip_value_and_type_oneline(tip_data, value_str, TIP_("3D Float Vector"));
}

static void build_tooltip_value_color(uiTooltipData &tip_data, const ColorGeometry4f &value)
{
  UI_tooltip_color_field_add(tip_data, float4(value), true, false, nullptr);
  add_space(tip_data);
  UI_tooltip_text_field_add(tip_data,
                            fmt::format("{}: {}", TIP_("Type"), TIP_("Float Color")),
                            {},
                            UI_TIP_STYLE_MONO,
                            UI_TIP_LC_VALUE);
}

static void build_tooltip_value_quaternion(uiTooltipData &tip_data, const math::Quaternion &value)
{
  const math::EulerXYZ euler = math::to_euler(value);
  const std::string value_str = fmt::format(
      "{}" BLI_STR_UTF8_DEGREE_SIGN " {}" BLI_STR_UTF8_DEGREE_SIGN " {}" BLI_STR_UTF8_DEGREE_SIGN,
      euler.x().degree(),
      euler.y().degree(),
      euler.z().degree());
  build_tooltip_value_and_type_oneline(tip_data, value_str, TIP_("Rotation"));
}

static void build_tooltip_value_bool(uiTooltipData &tip_data, const bool value)
{
  std::string value_str = value ? TIP_("True") : TIP_("False");
  build_tooltip_value_and_type_oneline(tip_data, value_str, TIP_("Boolean"));
}

static void build_tooltip_value_float4x4(uiTooltipData &tip_data, const float4x4 &value)
{
  /* Transpose to be able to print row by row. */
  const float4x4 value_transposed = math::transpose(value);

  std::stringstream ss;
  for (const int row_i : IndexRange(4)) {
    const float4 row = value_transposed[row_i];
    ss << fmt::format("{:7.3} {:7.3} {:7.3} {:7.3}\n", row[0], row[1], row[2], row[3]);
  }

  UI_tooltip_text_field_add(
      tip_data, fmt::format("{}:", TIP_("Value")), {}, UI_TIP_STYLE_MONO, UI_TIP_LC_VALUE);
  add_space(tip_data);
  UI_tooltip_text_field_add(tip_data, ss.str(), {}, UI_TIP_STYLE_MONO, UI_TIP_LC_VALUE);
  add_space(tip_data);
  UI_tooltip_text_field_add(tip_data,
                            fmt::format("{}: {}", TIP_("Type"), TIP_("4x4 Float Matrix")),
                            {},
                            UI_TIP_STYLE_MONO,
                            UI_TIP_LC_VALUE);
}

[[nodiscard]] static bool build_tooltip_value_generic(uiTooltipData &tip_data,
                                                      const bNodeSocket &socket,
                                                      const GPointer &value)

{
  const CPPType &value_type = *value.type();
  if (build_tooltip_value_data_block<Object>(tip_data, value)) {
    return true;
  }
  if (build_tooltip_value_data_block<Material>(tip_data, value)) {
    return true;
  }
  if (build_tooltip_value_data_block<Tex>(tip_data, value)) {
    return true;
  }
  if (build_tooltip_value_data_block<Image>(tip_data, value)) {
    return true;
  }
  if (build_tooltip_value_data_block<Collection>(tip_data, value)) {
    return true;
  }

  if (socket.type == SOCK_MENU) {
    if (!value_type.is<int>()) {
      return false;
    }
    const int item_identifier = *value.get<int>();
    build_tooltip_value_enum(tip_data, socket, item_identifier);
    return true;
  }

  const CPPType &socket_base_cpp_type = *socket.typeinfo->base_cpp_type;
  const bke::DataTypeConversions &conversions = bke::get_implicit_type_conversions();
  if (value_type != socket_base_cpp_type) {
    if (!conversions.is_convertible(value_type, socket_base_cpp_type)) {
      return false;
    }
  }
  BUFFER_FOR_CPP_TYPE_VALUE(socket_base_cpp_type, socket_value);
  conversions.convert_to_uninitialized(
      value_type, socket_base_cpp_type, value.get(), socket_value);
  BLI_SCOPED_DEFER([&]() { socket_base_cpp_type.destruct(socket_value); });

  if (socket_base_cpp_type.is<int>()) {
    build_tooltip_value_int(tip_data, *static_cast<int *>(socket_value));
    return true;
  }
  if (socket_base_cpp_type.is<float>()) {
    build_tooltip_value_float(tip_data, *static_cast<float *>(socket_value));
    return true;
  }
  if (socket_base_cpp_type.is<float3>()) {
    build_tooltip_value_float3(tip_data, *static_cast<float3 *>(socket_value));
    return true;
  }
  if (socket_base_cpp_type.is<ColorGeometry4f>()) {
    build_tooltip_value_color(tip_data, *static_cast<ColorGeometry4f *>(socket_value));
    return true;
  }
  if (socket_base_cpp_type.is<math::Quaternion>()) {
    build_tooltip_value_quaternion(tip_data, *static_cast<math::Quaternion *>(socket_value));
    return true;
  }
  if (socket_base_cpp_type.is<bool>()) {
    build_tooltip_value_bool(tip_data, *static_cast<bool *>(socket_value));
    return true;
  }
  if (socket_base_cpp_type.is<float4x4>()) {
    build_tooltip_value_float4x4(tip_data, *static_cast<float4x4 *>(socket_value));
    return true;
  }

  return false;
}

static bool build_tooltip_value_string_log(uiTooltipData &tip_data,
                                           const geo_log::StringLog &value_log)
{
  std::string value_str = value_log.value;
  if (value_log.truncated) {
    value_str += "...";
  }
  build_tooltip_value_and_type_oneline(tip_data, value_str, TIP_("String"));
  return true;
}

static const char *get_field_type_name(const CPPType &base_type)
{
  if (base_type.is<int>()) {
    return TIP_("Integer Field");
  }
  if (base_type.is<float>()) {
    return TIP_("Float Field");
  }
  if (base_type.is<blender::float3>()) {
    return TIP_("3D Float Vector Field");
  }
  if (base_type.is<bool>()) {
    return TIP_("Boolean Field");
  }
  if (base_type.is<std::string>()) {
    return TIP_("String Field");
  }
  if (base_type.is<blender::ColorGeometry4f>()) {
    return TIP_("Color Field");
  }
  if (base_type.is<math::Quaternion>()) {
    return TIP_("Rotation Field");
  }
  BLI_assert_unreachable();
  return TIP_("Field");
}

static bool build_tooltip_value_field_log(uiTooltipData &tip_data,
                                          const bNodeSocket &socket,
                                          const geo_log::FieldInfoLog &value_log)
{
  const CPPType &socket_base_cpp_type = *socket.typeinfo->base_cpp_type;
  const Span<std::string> input_tooltips = value_log.input_tooltips;

  if (input_tooltips.is_empty()) {
    /* Should have been logged as constant value. */
    BLI_assert_unreachable();
    return false;
  }

  UI_tooltip_text_field_add(
      tip_data, TIP_("Field dependending on:"), {}, UI_TIP_STYLE_MONO, UI_TIP_LC_VALUE);

  for (const std::string &input_tooltip : input_tooltips) {
    add_space(tip_data);
    UI_tooltip_text_field_add(
        tip_data, fmt::format("\u2022 {}", input_tooltip), {}, UI_TIP_STYLE_MONO, UI_TIP_LC_VALUE);
  }

  add_space(tip_data);
  std::string type_str = get_field_type_name(socket_base_cpp_type);
  UI_tooltip_text_field_add(tip_data,
                            fmt::format("{}: {}", TIP_("Type"), type_str),
                            {},
                            UI_TIP_STYLE_MONO,
                            UI_TIP_LC_VALUE);
  return true;
}

static std::string count_to_string(const int count)
{
  char str[BLI_STR_FORMAT_INT32_GROUPED_SIZE];
  BLI_str_format_int_grouped(str, count);
  return std::string(str);
}

static bool build_tooltip_value_geometry_log(uiTooltipData &tip_data,
                                             const geo_log::GeometryInfoLog &geometry_log)
{
  Span<bke::GeometryComponent::Type> component_types = geometry_log.component_types;
  if (component_types.is_empty()) {
    build_tooltip_value_and_type_oneline(tip_data, TIP_("None"), TIP_("Geometry Set"));
    return true;
  }
  UI_tooltip_text_field_add(
      tip_data, TIP_("Geometry components:"), {}, UI_TIP_STYLE_MONO, UI_TIP_LC_VALUE);
  for (const bke::GeometryComponent::Type type : component_types) {
    std::string component_str;
    switch (type) {
      case bke::GeometryComponent::Type::Mesh: {
        const geo_log::GeometryInfoLog::MeshInfo &info = *geometry_log.mesh_info;
        component_str = fmt::format(fmt::runtime(TIP_("Mesh: {} vertices, {} edges, {} faces")),
                                    count_to_string(info.verts_num),
                                    count_to_string(info.edges_num),
                                    count_to_string(info.faces_num));
        break;
      }
      case bke::GeometryComponent::Type::PointCloud: {
        const geo_log::GeometryInfoLog::PointCloudInfo &info = *geometry_log.pointcloud_info;
        component_str = fmt::format(fmt::runtime(TIP_("Point Cloud: {} points")),
                                    count_to_string(info.points_num));
        break;
      }
      case bke::GeometryComponent::Type::Instance: {
        const geo_log::GeometryInfoLog::InstancesInfo &info = *geometry_log.instances_info;
        component_str = fmt::format(fmt::runtime(TIP_("Instances: {}")),
                                    count_to_string(info.instances_num));
        break;
      }
      case bke::GeometryComponent::Type::Volume: {
        const geo_log::GeometryInfoLog::VolumeInfo &info = *geometry_log.volume_info;
        component_str = fmt::format(fmt::runtime(TIP_("Volume: {} grids")),
                                    count_to_string(info.grids_num));
        break;
      }
      case bke::GeometryComponent::Type::Curve: {
        const geo_log::GeometryInfoLog::CurveInfo &info = *geometry_log.curve_info;
        component_str = fmt::format(fmt::runtime(TIP_("Curve: {} points, {} splines")),
                                    count_to_string(info.points_num),
                                    count_to_string(info.splines_num));
        break;
      }
      case bke::GeometryComponent::Type::GreasePencil: {
        const geo_log::GeometryInfoLog::GreasePencilInfo &info = *geometry_log.grease_pencil_info;
        component_str = fmt::format(fmt::runtime(TIP_("Grease Pencil: {} layers")),
                                    count_to_string(info.layers_num));
        break;
      }
      case bke::GeometryComponent::Type::Edit: {
        if (geometry_log.edit_data_info.has_value()) {
          const geo_log::GeometryInfoLog::EditDataInfo &info = *geometry_log.edit_data_info;
          component_str = fmt::format(
              fmt::runtime(TIP_("Edit: {}, {}, {}")),
              info.has_deformed_positions ? TIP_("positions") : TIP_("no positions"),
              info.has_deform_matrices ? TIP_("matrices") : TIP_("no matrices"),
              info.gizmo_transforms_num > 0 ? TIP_("gizmos") : TIP_("no gizmos"));
        }
        break;
      }
    }
    if (!component_str.empty()) {
      add_space(tip_data);
      UI_tooltip_text_field_add(tip_data,
                                fmt::format("\u2022 {}", component_str),
                                {},
                                UI_TIP_STYLE_MONO,
                                UI_TIP_LC_VALUE);
    }
  }
  add_space(tip_data);
  UI_tooltip_text_field_add(
      tip_data, TIP_("Type: Geometry Set"), {}, UI_TIP_STYLE_MONO, UI_TIP_LC_VALUE);
  return true;
}

static bool build_tooltip_value_grid_log(uiTooltipData &tip_data,
                                         const geo_log::GridInfoLog &grid_log)
{
  std::string value_str;
  if (grid_log.is_empty) {
    value_str = TIP_("None");
  }
  else {
    value_str = TIP_("Grid");
  }
  build_tooltip_value_and_type_oneline(tip_data, value_str, TIP_("Grid"));
  return true;
}

static bool build_tooltip_value_bundle_log(uiTooltipData &tip_data,
                                           const geo_log::BundleValueLog &bundle_log)
{
  if (bundle_log.items.is_empty()) {
    UI_tooltip_text_field_add(
        tip_data, TIP_("Values: None"), {}, UI_TIP_STYLE_MONO, UI_TIP_LC_VALUE);
  }
  else {
    UI_tooltip_text_field_add(tip_data, TIP_("Values:"), {}, UI_TIP_STYLE_MONO, UI_TIP_LC_VALUE);
    Vector<geo_log::BundleValueLog::Item> sorted_items = bundle_log.items;
    std::sort(sorted_items.begin(), sorted_items.end(), [](const auto &a, const auto &b) {
      return BLI_strcasecmp_natural(a.key.identifiers().first().c_str(),
                                    b.key.identifiers().first().c_str()) < 0;
    });
    for (const geo_log::BundleValueLog::Item &item : sorted_items) {
      add_space(tip_data);
      const std::string type_name = TIP_(item.type->label);
      UI_tooltip_text_field_add(tip_data,
                                fmt::format(fmt::runtime("\u2022 \"{}\" ({})\n"),
                                            item.key.identifiers().first(),
                                            type_name),
                                {},
                                UI_TIP_STYLE_MONO,
                                UI_TIP_LC_VALUE);
    }
  }
  add_space(tip_data);
  UI_tooltip_text_field_add(
      tip_data, TIP_("Type: Bundle"), {}, UI_TIP_STYLE_MONO, UI_TIP_LC_VALUE);
  return true;
}

static bool build_tooltip_value_closure_log(uiTooltipData &tip_data,
                                            const geo_log::ClosureValueLog &closure_log)
{
  if (closure_log.inputs.is_empty() && closure_log.outputs.is_empty()) {
    UI_tooltip_text_field_add(
        tip_data, TIP_("Value: None"), {}, UI_TIP_STYLE_MONO, UI_TIP_LC_VALUE);
  }
  else {
    if (!closure_log.inputs.is_empty()) {
      UI_tooltip_text_field_add(tip_data, TIP_("Inputs:"), {}, UI_TIP_STYLE_MONO, UI_TIP_LC_VALUE);
      for (const geo_log::ClosureValueLog::Item &item : closure_log.inputs) {
        add_space(tip_data);
        const std::string type_name = TIP_(item.type->label);
        UI_tooltip_text_field_add(tip_data,
                                  fmt::format(fmt::runtime("\u2022 \"{}\" ({})\n"),
                                              item.key.identifiers().first(),
                                              type_name),
                                  {},
                                  UI_TIP_STYLE_MONO,
                                  UI_TIP_LC_VALUE);
      }
    }
    if (!closure_log.outputs.is_empty()) {
      add_space(tip_data);
      UI_tooltip_text_field_add(
          tip_data, TIP_("Outputs:"), {}, UI_TIP_STYLE_MONO, UI_TIP_LC_VALUE);
      for (const geo_log::ClosureValueLog::Item &item : closure_log.outputs) {
        add_space(tip_data);
        const std::string type_name = TIP_(item.type->label);
        UI_tooltip_text_field_add(tip_data,
                                  fmt::format(fmt::runtime("\u2022 \"{}\" ({})\n"),
                                              item.key.identifiers().first(),
                                              type_name),
                                  {},
                                  UI_TIP_STYLE_MONO,
                                  UI_TIP_LC_VALUE);
      }
    }
  }
  add_space(tip_data);
  UI_tooltip_text_field_add(
      tip_data, TIP_("Type: Closure"), {}, UI_TIP_STYLE_MONO, UI_TIP_LC_VALUE);
  return true;
}

[[nodiscard]] static bool build_tooltip_value_geo_log(uiTooltipData &tip_data,
                                                      const bNodeSocket &socket,
                                                      geo_log::ValueLog &value_log)
{
  if (const auto *generic_value_log = dynamic_cast<const geo_log::GenericValueLog *>(&value_log)) {
    return build_tooltip_value_generic(tip_data, socket, generic_value_log->value);
  }
  if (const auto *string_value_log = dynamic_cast<const geo_log::StringLog *>(&value_log)) {
    return build_tooltip_value_string_log(tip_data, *string_value_log);
  }
  if (const auto *field_value_log = dynamic_cast<const geo_log::FieldInfoLog *>(&value_log)) {
    return build_tooltip_value_field_log(tip_data, socket, *field_value_log);
  }
  if (const auto *geometry_log = dynamic_cast<const geo_log::GeometryInfoLog *>(&value_log)) {
    return build_tooltip_value_geometry_log(tip_data, *geometry_log);
  }
  if (const auto *grid_log = dynamic_cast<const geo_log::GridInfoLog *>(&value_log)) {
    return build_tooltip_value_grid_log(tip_data, *grid_log);
  }
  if (const auto *bundle_log = dynamic_cast<const geo_log::BundleValueLog *>(&value_log)) {
    return build_tooltip_value_bundle_log(tip_data, *bundle_log);
  }
  if (const auto *closure_log = dynamic_cast<const geo_log::ClosureValueLog *>(&value_log)) {
    return build_tooltip_value_closure_log(tip_data, *closure_log);
  }
  return true;
}

static void build_tooltip_value_unknown(uiTooltipData &tip_data, const bNodeSocket & /*socket*/)
{
  build_tooltip_value_and_type_oneline(tip_data, TIP_("Unknown"), TIP_("Unknown"));
}

static bool build_tooltip_last_value_multi_input(uiTooltipData &tip_data,
                                                 geo_log::GeoTreeLog &geo_tree_log,
                                                 const bNodeSocket &socket)
{
  const Span<const bNodeLink *> connected_links = socket.directly_linked_links();
  bool is_first = true;
  for (const int i : connected_links.index_range()) {
    const bNodeLink &link = *connected_links[i];
    if (!link.is_used()) {
      continue;
    }
    if (!(link.flag & NODE_LINK_VALID)) {
      continue;
    }
    const bNodeSocket &from_socket = *link.fromsock;
    const int connection_number = i + 1;
    add_space(tip_data, is_first ? 0 : 2);
    is_first = false;
    UI_tooltip_text_field_add(tip_data,
                              fmt::format("{}:", connection_number),
                              {},
                              UI_TIP_STYLE_NORMAL,
                              UI_TIP_LC_NORMAL);
    add_space(tip_data);
    if (geo_log::ValueLog *value_log = geo_tree_log.find_socket_value_log(from_socket)) {
      build_tooltip_value_geo_log(tip_data, socket, *value_log);
    }
    else {
      build_tooltip_value_unknown(tip_data, socket);
    }
  }

  return true;
}

[[nodiscard]] static bool build_tooltip_last_value(uiTooltipData &tip_data,
                                                   geo_log::GeoTreeLog *geo_tree_log,
                                                   const bNodeSocket &socket)

{
  if (!geo_tree_log) {
    return false;
  }
  if (socket.typeinfo->base_cpp_type == nullptr) {
    return false;
  }
  geo_tree_log->ensure_socket_values();
  if (socket.is_multi_input()) {
    return build_tooltip_last_value_multi_input(tip_data, *geo_tree_log, socket);
  }
  geo_log::ValueLog *value_log = geo_tree_log->find_socket_value_log(socket);
  if (!value_log) {
    return false;
  }
  return build_tooltip_value_geo_log(tip_data, socket, *value_log);
}

static void build_tooltip_value_socket_default(uiTooltipData &tip_data, const bNodeSocket &socket)
{
  if (socket.is_multi_input()) {
    /* TODO */
    return;
  }
  if (socket.owner_node().is_reroute()) {
    /* TODO */
    return;
  }
  const nodes::SocketDeclaration *socket_decl = socket.runtime->declaration;
  if (socket_decl && socket_decl->input_field_type == nodes::InputSocketFieldType::Implicit) {
    /* TODO */
    return;
  }
  if (socket.typeinfo->base_cpp_type == nullptr) {
    return;
  }
  const CPPType &cpp_type = *socket.typeinfo->base_cpp_type;
  BUFFER_FOR_CPP_TYPE_VALUE(cpp_type, socket_value);
  socket.typeinfo->get_base_cpp_value(socket.default_value, socket_value);
  BLI_SCOPED_DEFER([&]() { cpp_type.destruct(socket_value); });
  if (!build_tooltip_value_generic(tip_data, socket, {cpp_type, socket_value})) {
    build_tooltip_value_and_type_oneline(tip_data, TIP_("Unknown"), TIP_("Unknown"));
  }
}

static bool is_socket_default_value_used(const bNodeSocket &socket)
{
  BLI_assert(socket.is_input());
  for (const bNodeLink *link : socket.directly_linked_links()) {
    if (!link->is_used()) {
      continue;
    }
    const bNodeSocket &from_socket = *link->fromsock;
    const bNode &from_node = from_socket.owner_node();
    if (from_node.is_dangling_reroute()) {
      continue;
    }
    return false;
  }
  return true;
}

static void build_tooltip_last_value(uiTooltipData &tip_data,
                                     bContext &C,
                                     const bNodeSocket &socket)
{
  SpaceNode *snode = CTX_wm_space_node(&C);
  const bNode &node = socket.owner_node();

  geo_log::ContextualGeoTreeLogs geo_tree_logs;
  if (snode) {
    geo_tree_logs = geo_log::GeoNodesLog::get_contextual_tree_logs(*snode);
  }
  geo_log::GeoTreeLog *geo_tree_log = geo_tree_logs.get_main_tree_log(socket);
  if (build_tooltip_last_value(tip_data, geo_tree_log, socket)) {
    return;
  }
  if (node.is_reroute()) {
    build_tooltip_value_unknown(tip_data, socket);
    return;
  }
  if (socket.is_input()) {
    if (is_socket_default_value_used(socket)) {
      build_tooltip_value_socket_default(tip_data, socket);
      return;
    }
  }
  build_tooltip_value_unknown(tip_data, socket);
}

void build_socket_tooltip(uiTooltipData &tip_data,
                          bContext &C,
                          const bNodeTree & /*tree*/,
                          const bNodeSocket &socket)
{
  build_tooltip_label(tip_data, socket);
  build_tooltip_description(tip_data, socket);
  add_space(tip_data, 2);
  build_tooltip_last_value(tip_data, C, socket);
  add_space(tip_data, 2);

  /* Dangling reroute. */
  /* Add allowed type. */
}

}  // namespace blender::ed::space_node
