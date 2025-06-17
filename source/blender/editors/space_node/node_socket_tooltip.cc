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
  if (!storage->enum_items) {
    return;
  }
  if (storage->has_conflict()) {
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

[[nodiscard]] static bool build_tooltip_value_geo_log(uiTooltipData &tip_data,
                                                      const bNodeSocket &socket,
                                                      geo_log::ValueLog &value_log)
{
  if (const auto *generic_value_log = dynamic_cast<const geo_log::GenericValueLog *>(&value_log)) {
    return build_tooltip_value_generic(tip_data, socket, generic_value_log->value);
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
  geo_log::ValueLog *value_log = geo_tree_log->find_socket_value_log(socket);
  if (!value_log) {
    return false;
  }
  return build_tooltip_value_geo_log(tip_data, socket, *value_log);
}

static void build_tooltip_last_value(uiTooltipData &tip_data,
                                     bContext &C,
                                     const bNodeSocket &socket)
{
  SpaceNode *snode = CTX_wm_space_node(&C);

  geo_log::ContextualGeoTreeLogs geo_tree_logs;
  if (snode) {
    geo_tree_logs = geo_log::GeoNodesLog::get_contextual_tree_logs(*snode);
  }
  geo_log::GeoTreeLog *geo_tree_log = geo_tree_logs.get_main_tree_log(socket);
  if (!build_tooltip_last_value(tip_data, geo_tree_log, socket)) {
  }
}

void build_socket_tooltip(uiTooltipData &tip_data,
                          bContext &C,
                          const bNodeTree & /*tree*/,
                          const bNodeSocket &socket)
{
  build_tooltip_label(tip_data, socket);
  add_space(tip_data, 2);
  build_tooltip_description(tip_data, socket);
  build_tooltip_last_value(tip_data, C, socket);

  /* Add allowed type. */
}

}  // namespace blender::ed::space_node
