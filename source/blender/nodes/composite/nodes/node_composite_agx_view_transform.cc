/* SPDX-FileCopyrightText: 2025 Blender Authors */
/* SPDX-License-Identifier: GPL-2.0-or-later */

// Include Headers
#include "BKE_node.hh"
#include "BLI_math_base.hh"
#include "BLI_math_color.h"
#include "BLI_math_color.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_string.h"
#include "BLT_translation.hh"
#include "COM_node_operation.hh"
#include "DNA_node_types.h"
#include "FN_multi_function_builder.hh"
#include "GPU_material.hh"
#include "IMB_colormanagement.hh"
#include "NOD_multi_function.hh"
#include "NOD_node_declaration.hh"
#include "NOD_rna_define.hh"
#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "node_cmp_agx_utils.hh"
#include "node_composite_util.hh"

// Namespace Declaration
using namespace blender::compositor;

namespace blender::nodes::node_composite_agx_view_transform_cc {

// define enums
enum class AGXPrimaries : int16_t {
  AGX_PRIMARIES_P3D65 = 0,
  AGX_PRIMARIES_REC709 = 1,
  AGX_PRIMARIES_REC2020 = 2,
  AGX_PRIMARIES_AWG3 = 3,
  AGX_PRIMARIES_AWG4 = 4,
  AGX_PRIMARIES_EGAMUT = 5,
};

enum class AGXWorkingLog : int16_t {
  AGX_WORKING_LOG_TLOG = 0,
  AGX_WORKING_LOG_ARRI_LOGC3 = 1,
  AGX_WORKING_LOG_ARRI_LOGC4 = 2,
  AGX_WORKING_LOG_GENERIC_LOG2 = 3,
};

static const EnumPropertyItem agx_working_primaries_items[] = {
    {int(AGXPrimaries::AGX_PRIMARIES_P3D65), "p3d65", 0, "P3-D65", ""},
    {int(AGXPrimaries::AGX_PRIMARIES_REC709), "rec709", 0, "Rec.709", ""},
    {int(AGXPrimaries::AGX_PRIMARIES_REC2020), "rec2020", 0, "Rec.2020", ""},
    {int(AGXPrimaries::AGX_PRIMARIES_AWG3), "awg3", 0, "ARRI Alexa Wide Gamut 3", ""},
    {int(AGXPrimaries::AGX_PRIMARIES_AWG4), "awg4", 0, "ARRI Alexa Wide Gamut 4", ""},
    {int(AGXPrimaries::AGX_PRIMARIES_EGAMUT), "egamut", 0, "FilmLight E-Gamut", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem agx_display_primaries_items[] = {
    {int(AGXPrimaries::AGX_PRIMARIES_P3D65), "p3d65", 0, "P3-D65", ""},
    {int(AGXPrimaries::AGX_PRIMARIES_REC709), "rec709", 0, "Rec.709", ""},
    {int(AGXPrimaries::AGX_PRIMARIES_REC2020), "rec2020", 0, "Rec.2020", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem agx_working_log_items[] = {
    {int(AGXWorkingLog::AGX_WORKING_LOG_TLOG), "t_log", 0, "FilmLight T-Log", ""},
    {int(AGXWorkingLog::AGX_WORKING_LOG_ARRI_LOGC3), "arri_logc3", 0, "ARRI LogC3", ""},
    {int(AGXWorkingLog::AGX_WORKING_LOG_ARRI_LOGC4), "arri_logc4", 0, "ARRI LogC4", ""},
    {int(AGXWorkingLog::AGX_WORKING_LOG_GENERIC_LOG2), "generic_log2", 0, "Generic Log2", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

// RNA functions for node properties
static void node_rna(StructRNA *srna)
{
  PropertyRNA *prop;

  prop = RNA_def_node_enum(srna,
                           "working_primaries",
                           "Working",
                           "The working primaries that the AgX mechanism applies to",
                           agx_working_primaries_items,
                           NOD_inline_enum_accessors(custom2),
                           int(AGXPrimaries::AGX_PRIMARIES_REC2020));

  prop = RNA_def_node_enum(srna,
                           "working_log",
                           "Log",
                           "The Log curve applied before the sigmoid in the AgX mechanism",
                           agx_working_log_items,
                           NOD_inline_enum_accessors(custom3),
                           int(AGXWorkingLog::AGX_WORKING_LOG_GENERIC_LOG2));

  prop = RNA_def_node_enum(srna,
                           "display_primaries",
                           "Display",
                           "The primaries of the target display device",
                           agx_display_primaries_items,
                           NOD_inline_enum_accessors(custom4),
                           int(AGXPrimaries::AGX_PRIMARIES_REC709));

  prop = RNA_def_node_boolean(
      srna,
      "use_pre_for_post",
      "Use Pre for Post",
      "Use pre-processing section's settings for post-processing, for ease of use",
      NOD_inline_boolean_accessors(custom1, 1),
      true);
}

// initialize
static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom2 = int(AGXPrimaries::AGX_PRIMARIES_REC2020);
  node->custom3 = int(AGXWorkingLog::AGX_WORKING_LOG_GENERIC_LOG2);
  node->custom4 = int(AGXPrimaries::AGX_PRIMARIES_REC709);
  node->custom1 = true;
}

// Node Declaration
static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_output<decl::Color>("Color").structure_type(StructureType::Dynamic);

  b.add_input<decl::Color>("Color")
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .structure_type(StructureType::Dynamic);

  /* Panel for log and sigmoid curve settings. */
  PanelDeclarationBuilder &curve_panel = b.add_panel("Curve").default_closed(true);

  curve_panel.add_input<decl::Float>("General Contrast")
      .default_value(2.4f)
      .min(1.4f)
      .max(4.0f)
      .subtype(PROP_FACTOR)
      .description(
          "Slope of the S curve. "
          "Control the general contrast across the image")
      .structure_type(StructureType::Single);

  curve_panel.add_input<decl::Float>("Toe Contrast")
      .default_value(1.5f)
      .min(0.7f)
      .max(10.0f)
      .subtype(PROP_FACTOR)
      .description(
          "Toe exponential power of the S curve. "
          "Higher values make darker regions crush harder towards floor")
      .structure_type(StructureType::Single);

  curve_panel.add_input<decl::Float>("Shoulder Contrast")
      .default_value(1.5f)
      .min(0.7f)
      .max(10.0f)
      .subtype(PROP_FACTOR)
      .description(
          "Shoulder exponential power of the S curve. "
          "Higher values make brighter regions crush harder towards ceiling")
      .structure_type(StructureType::Single);

  curve_panel.add_input<decl::Float>("Middle Gray")
      .default_value(18.0f)
      .min(10.0f)
      .max(25.0f)
      .subtype(PROP_PERCENTAGE)
      .short_label("MidGray")
      .description("Middle Gray percentage in the linearized formed picture")
      .structure_type(StructureType::Single);

  curve_panel.add_layout([](uiLayout *layout, bContext * /*C*/, PointerRNA *ptr) {
    layout->prop(ptr, "working_log", UI_ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  });

  curve_panel.add_input<decl::Float>("Log2 Minimum Exposure")
      .default_value(-10.0f)
      .min(-15.0f)
      .max(-5.0f)
      .subtype(PROP_NONE)
      .short_label("Log2 Min")
      .description("Minimum Exposure to form the picture. Values are in Exposure stops")
      .structure_type(StructureType::Single);

  curve_panel.add_input<decl::Float>("Log2 Maximum Exposure")
      .default_value(6.5f)
      .min(4.0f)
      .max(15.0f)
      .subtype(PROP_NONE)
      .short_label("Log2 Max")
      .description("Maximum Exposure to form the picture. Values are in Exposure stops")
      .structure_type(StructureType::Single);

  curve_panel.add_input<decl::Bool>("Maintain General Contrast")
      .default_value(true)
      .short_label("Maintain Contrast")
      .description("Maintain General Contrast when Log2 range changes, for ease of use")
      .structure_type(StructureType::Single);

  /* Panel for inset matrix settings. */
  PanelDeclarationBuilder &inset_panel = b.add_panel("Pre-Processing").default_closed(false);

  inset_panel.add_input<decl::Vector>("RGB Rotation")
      .default_value({0.0373458572f, -0.0214373556f, -0.05326291091f})
      .min(-0.26179938779f)
      .max(0.26179938779f)
      .subtype(PROP_EULER)
      .description(
          "Rotate RGB primaries in the exposure state. "
          "Negative is clockwise, and positive is counterclockwise. "
          "Affect direction of the hue-bending")
      .structure_type(StructureType::Single);

  inset_panel.add_input<decl::Vector>("Purity Attenuation")
      .default_value({32.9652f, 28.0513f, 12.4754f})
      .min(0.0f)
      .max(60.0f)
      .subtype(PROP_PERCENTAGE)
      .description(
          "Rate of the whitening of the picture. "
          "Lower values result in less whitening and more hue-bending at the risk of potential "
          "artifacts occuring")
      .structure_type(StructureType::Single);

  inset_panel.add_layout([](uiLayout *layout, bContext * /*C*/, PointerRNA *ptr) {
    layout->prop(ptr, "use_pre_for_post", UI_ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  });

  /* Panel for outset matrix settings. */
  PanelDeclarationBuilder &outset_panel = b.add_panel("Post-Processing").default_closed(true);

  outset_panel.add_input<decl::Vector>("Reverse RGB Rotation")
      .default_value({0.0f, 0.0f, 0.0f})
      .min(-0.26179938779f)
      .max(0.26179938779f)
      .subtype(PROP_EULER)
      .description(
          "Reverse the primaries rotation as post-processing after forming the picture. "
          "Direction is the reverse of the Attenuation. Negative is counterclockwise, positive is "
          "clockwise")
      .structure_type(StructureType::Single);

  outset_panel.add_input<decl::Vector>("Restore Purity")
      .default_value({32.3174f, 28.3256f, 3.7433f})
      .min(0.0f)
      .max(60.0f)
      .subtype(PROP_PERCENTAGE)
      .description(
          "Restore the purity of the formed picture as a post-processing. "
          "Higher values make the image look more chroma-laden at the risk of potential artifacts")
      .structure_type(StructureType::Single);

  /* Panel for look adjustments settings. */
  PanelDeclarationBuilder &look_panel = b.add_panel("Look").default_closed(false);

  look_panel.add_input<decl::Float>("Hue Flight Strength")
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description(
          "Strength of the hue-bending introduced by the per-channel curve. "
          "Higher value will have yellower orange, while lower value would turn fire salmonish, "
          "for example")
      .structure_type(StructureType::Single);

  look_panel.add_input<decl::Float>("Tinting Scale")
      .default_value(0.0f)
      .min(-0.2f)
      .max(0.2f)
      .subtype(PROP_FACTOR)
      .description(
          "Control how far the white point shifts in the restoration step. "
          "Affecting the strength of the post-processing tinting effect")
      .structure_type(StructureType::Single);

  look_panel.add_input<decl::Float>("Tinting Hue")
      .default_value(0.0f)
      .min(-3.14159265358979323846f)
      .max(3.14159265358979323846f)
      .subtype(PROP_ANGLE)
      .description(
          "Adjust the direction in which the white point shifts in the restoration step. "
          "influencing the overall hue of the tinting effect")
      .structure_type(StructureType::Single);

  /* Panel for primaries settings. */
  PanelDeclarationBuilder &primaries_panel = b.add_panel("Primaries").default_closed(true);

  primaries_panel.add_layout([](uiLayout *layout, bContext * /*C*/, PointerRNA *ptr) {
    layout->prop(ptr, "working_primaries", UI_ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
    layout->prop(ptr, "display_primaries", UI_ITEM_R_SPLIT_EMPTY_NAME, std::nullopt, ICON_NONE);
  });

  primaries_panel.add_input<decl::Bool>("Compensate for the Negatives")
      .default_value(true)
      .short_label("Compensate Negatives")
      .description(
          "Use special luminance compensation technique to prevent out-of-gamut negative values. "
          "Done in both RGB exposure state and formed picture state")
      .structure_type(StructureType::Single);

  /* Panel for HDR settings. */
  PanelDeclarationBuilder &hdr_panel = b.add_panel("HDR").default_closed(true);

  hdr_panel.add_input<decl::Bool>("Use HDR")
      .default_value(false)
      .description("Enable HDR logic")
      .structure_type(StructureType::Single);

  hdr_panel.add_input<decl::Float>("HDR Peak")
      .default_value(1000.0f)
      .min(400.0f)
      .max(1000.0f)
      .subtype(PROP_FACTOR)
      .description("Target peak display nits value of the picture")
      .structure_type(StructureType::Single);

  hdr_panel.add_input<decl::Float>("HDR Middle Gray Factor")
      .default_value(100.0f)
      .min(100.0f)
      .max(400.0f)
      .subtype(PROP_FACTOR)
      .short_label("HDR MidGray Factor")
      .description("The display nits value that MidGray percentage is relative to")
      .structure_type(StructureType::Single);

  hdr_panel.add_input<decl::Float>("HDR Purity")
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Purity of the \"HDR headroom\" ")
      .structure_type(StructureType::Single);
}

static void node_update(bNodeTree *ntree, bNode *node)
{
  // ---- hide sockets based on rna properties ----
  // Get the value of the boolean property
  bool use_same_settings = node->custom1;
  bool outset_panel_sockets_available = !use_same_settings;
  // Find and set the availability of each related socket
  bNodeSocket *reverse_rgb_rotation_soc = blender::bke::node_find_socket(
      *node, SOCK_IN, "Reverse RGB Rotation");
  if (reverse_rgb_rotation_soc) {
    blender::bke::node_set_socket_availability(
        *ntree, *reverse_rgb_rotation_soc, outset_panel_sockets_available);
  }

  bNodeSocket *restore_purity_soc = blender::bke::node_find_socket(
      *node, SOCK_IN, "Restore Purity");
  if (restore_purity_soc) {
    blender::bke::node_set_socket_availability(
        *ntree, *restore_purity_soc, outset_panel_sockets_available);
  }

  // Get the value of the enum property
  bool log2_settings_available = node->custom3 == int(AGXWorkingLog::AGX_WORKING_LOG_GENERIC_LOG2);
  // Find and set the availability of each related socket
  bNodeSocket *log2_exposure_min_soc = blender::bke::node_find_socket(
      *node, SOCK_IN, "Log2 Minimum Exposure");
  if (log2_exposure_min_soc) {
    blender::bke::node_set_socket_availability(
        *ntree, *log2_exposure_min_soc, log2_settings_available);
  }

  bNodeSocket *log2_exposure_max_soc = blender::bke::node_find_socket(
      *node, SOCK_IN, "Log2 Maximum Exposure");
  if (log2_exposure_max_soc) {
    blender::bke::node_set_socket_availability(
        *ntree, *log2_exposure_max_soc, log2_settings_available);
  }

  bNodeSocket *log2_maintain_contrast_soc = blender::bke::node_find_socket(
      *node, SOCK_IN, "Maintain General Contrast");
  if (log2_maintain_contrast_soc) {
    blender::bke::node_set_socket_availability(
        *ntree, *log2_maintain_contrast_soc, log2_settings_available);
  }
}

// define precomputation logic
struct AgxPrecomputeData {
  float3x3 scene_linear_to_working;
  float3x3 working_to_display;
  float3x3 display_to_scene_linear;
  float log_midgray;
  float display_native_midgray;
  float3x3 insetmat;
  float3x3 outsetmat;
  float3x3 working_to_rec2020;
  float3x3 display_to_rec2020;
};

static AgxPrecomputeData agx_precompute(const float log2_min_in,
                                        const float log2_max_in,
                                        const float linear_midgray_percentage_in,
                                        const float3 rgb_rotation_in,
                                        const float3 attenuation_rates_in,
                                        const float3 reverse_rgb_rotation_in,
                                        const float3 restore_purity_in,
                                        const float tinting_scale_in,
                                        const float tinting_hue_in,
                                        const bool p_use_inverse_inset,
                                        const int p_working_primaries,
                                        const int p_working_log,
                                        const int p_display_primaries)
{
  // precompute conversion matrices
  const float3x3 scene_to_xyz = IMB_colormanagement_get_scene_linear_to_xyz();
  const float3x3 xyz_to_scene = IMB_colormanagement_get_xyz_to_scene_linear();

  float3x3 xyz_to_working = XYZtoRGB(COLOR_SPACE_PRI[static_cast<int>(p_working_primaries)]);
  const float3x3 scene_linear_to_working = xyz_to_working * scene_to_xyz;

  const float3x3 working_to_display = RGBtoRGB(
      COLOR_SPACE_PRI[static_cast<int>(p_working_primaries)],
      COLOR_SPACE_PRI[static_cast<int>(p_display_primaries)]);

  float3x3 display_to_xyz = RGBtoXYZ(COLOR_SPACE_PRI[static_cast<int>(p_display_primaries)]);
  const float3x3 display_to_scene_linear = xyz_to_scene * display_to_xyz;

  // precompute log middle gray
  float log_midgray = lin2log(float3(0.18f, 0.18f, 0.18f),
                              static_cast<int>(p_working_log),
                              log2_min_in,
                              log2_max_in)
                          .x;
  // precompute display native middle gray
  float display_native_power = 2.4f;
  float lin_midgray_float = linear_midgray_percentage_in / 100.0f;
  const float display_native_midgray = pow(lin_midgray_float, 1.0f / display_native_power);
  // precompute inset matrix
  Chromaticities inset_chromaticities = InsetPrimaries(
      COLOR_SPACE_PRI[static_cast<int>(p_working_primaries)],
      attenuation_rates_in.x,
      attenuation_rates_in.y,
      attenuation_rates_in.z,
      rgb_rotation_in.x,
      rgb_rotation_in.y,
      rgb_rotation_in.z);

  float3x3 insetmat = RGBtoRGB(inset_chromaticities,
                               COLOR_SPACE_PRI[static_cast<int>(p_working_primaries)]);

  // precompute outset matrix
  float3x3 outset_mat_inv_inset;
  float3x3 outset_mat;

  Chromaticities outset_inv_inset_chromaticities = InsetPrimaries(
      COLOR_SPACE_PRI[static_cast<int>(p_working_primaries)],
      attenuation_rates_in.x,
      attenuation_rates_in.y,
      attenuation_rates_in.z,
      rgb_rotation_in.x,
      rgb_rotation_in.y,
      rgb_rotation_in.z,
      /* Above parameters use pre-processing settings */
      tinting_hue_in + 3.14159265358979323846,
      tinting_scale_in);
  outset_mat_inv_inset = blender::math::invert(RGBtoRGB(
      outset_inv_inset_chromaticities, COLOR_SPACE_PRI[static_cast<int>(p_working_primaries)]));

  Chromaticities outset_chromaticities = InsetPrimaries(
      COLOR_SPACE_PRI[static_cast<int>(p_working_primaries)],
      restore_purity_in.x,
      restore_purity_in.y,
      restore_purity_in.z,
      reverse_rgb_rotation_in.x,
      reverse_rgb_rotation_in.y,
      reverse_rgb_rotation_in.z,
      tinting_hue_in + 3.14159265358979323846,
      tinting_scale_in);
  outset_mat = blender::math::invert(
      RGBtoRGB(outset_chromaticities, COLOR_SPACE_PRI[static_cast<int>(p_working_primaries)]));

  float3x3 outsetmat = p_use_inverse_inset ? outset_mat_inv_inset : outset_mat;

  const float3x3 working_to_rec2020 = RGBtoRGB(
      COLOR_SPACE_PRI[static_cast<int>(p_working_primaries)], REC2020_PRI);
  const float3x3 display_to_rec2020 = RGBtoRGB(
      COLOR_SPACE_PRI[static_cast<int>(p_display_primaries)], REC2020_PRI);

  return AgxPrecomputeData{scene_linear_to_working,
                           working_to_display,
                           display_to_scene_linear,
                           log_midgray,
                           display_native_midgray,
                           insetmat,
                           outsetmat,
                           working_to_rec2020,
                           display_to_rec2020};
}

// CPU processing logic
static float4 agx_image_formation(const float4 color,
                                  const float general_contrast_in,
                                  const float toe_contrast_in,
                                  const float shoulder_contrast_in,
                                  const float linear_midgray_percentage_in,
                                  const float log2_min_in,
                                  const float log2_max_in,
                                  const bool log2_maintain_contrast_in,
                                  const float3 rgb_rotation_in,
                                  const float3 attenuation_rates_in,
                                  const float3 reverse_rgb_rotation_in,
                                  const float3 restore_purity_in,
                                  const float hue_flight_strength_in,
                                  const float tinting_scale_in,
                                  const float tinting_hue_in,
                                  const bool compensate_negatives_in,
                                  const bool use_hdr_in,
                                  const float hdr_peak_in,
                                  const float hdr_midgray_factor_in,
                                  const float hdr_purity_in,
                                  const int p_working_primaries,
                                  const int p_working_log,
                                  const int p_use_inverse_inset,
                                  const float3x3 scene_linear_to_working,
                                  const float3x3 working_to_display,
                                  const float3x3 display_to_scene_linear,
                                  const float log_midgray_precomputed,
                                  const float display_native_midgray_precomputed,
                                  const float3x3 insetmat_precomputed,
                                  const float3x3 outsetmat_precomputed,
                                  const float3x3 working_to_rec2020,
                                  const float3x3 display_to_rec2020)
{
  float3 rgb;
  rgb.x = color.x;
  rgb.y = color.y;
  rgb.z = color.z;

  rgb = scene_linear_to_working * rgb;

  // apply low-side guard rail if the UI checkbox is true, otherwise hard clamp to 0
  if (compensate_negatives_in) {
    rgb = compensate_low_side(rgb, false, working_to_rec2020);
  }
  else {
    rgb = maxf3(0, rgb);
  }

  // check whether inset matrix is precomputed, if not, calculate it per-pixel
  float3x3 insetmat_perpixel;
  if (std::isnan(insetmat_precomputed[0][0])) {
    Chromaticities inset_chromaticities = InsetPrimaries(
        COLOR_SPACE_PRI[static_cast<int>(p_working_primaries)],
        attenuation_rates_in.x,
        attenuation_rates_in.y,
        attenuation_rates_in.z,
        rgb_rotation_in.x,
        rgb_rotation_in.y,
        rgb_rotation_in.z);
    insetmat_perpixel = RGBtoRGB(inset_chromaticities,
                                 COLOR_SPACE_PRI[static_cast<int>(p_working_primaries)]);
  }

  // apply inset matrix
  if (std::isnan(insetmat_precomputed[0][0])) {
    rgb = insetmat_perpixel * rgb;
  }
  else {
    rgb = insetmat_precomputed * rgb;
  }
  // record pre-formation chromaticity angle
  float3 pre_curve_hsv;
  rgb_to_hsv_v(rgb, pre_curve_hsv);

  // encode to working log
  rgb = lin2log(rgb, static_cast<int>(p_working_log), log2_min_in, log2_max_in);
  float general_contrast = general_contrast_in;
  if (log2_maintain_contrast_in &&
      p_working_log == int(AGXWorkingLog::AGX_WORKING_LOG_GENERIC_LOG2))
  {
    float base_exposure_range = 16.5f;
    general_contrast = ((fabsf(log2_min_in) + log2_max_in) / base_exposure_range) *
                       general_contrast_in;
  }

  // check whether log middle gray is precomputed, if no, calculate it per-pixel.
  float log_midgray_perpixel;
  if (std::isnan(log_midgray_precomputed)) {
    log_midgray_perpixel = lin2log(float3(0.18f, 0.18f, 0.18f),
                                   static_cast<int>(p_working_log),
                                   log2_min_in,
                                   log2_max_in)
                               .x;
  }

  // check whether log middle gray is precomputed, if no, calculate it per-pixel.
  float display_native_midgray_perpixel;
  if (std::isnan(display_native_midgray_precomputed)) {
    float display_native_power = 2.4f;
    float lin_midgray_float = linear_midgray_percentage_in / 100.0f;
    display_native_midgray_perpixel = pow(lin_midgray_float, 1.0f / display_native_power);
  }

  float log_midgray_applied;
  float display_native_midgray_applied;

  if (std::isnan(log_midgray_precomputed)) {
    log_midgray_applied = log_midgray_perpixel;
  }
  else {
    log_midgray_applied = log_midgray_precomputed;
  }

  if (std::isnan(display_native_midgray_precomputed)) {
    display_native_midgray_applied = display_native_midgray_perpixel;
  }
  else {
    display_native_midgray_applied = display_native_midgray_precomputed;
  }

  float hdr_sdr_ratio = hdr_peak_in / hdr_midgray_factor_in;
  float shoulder_contrast_applied = shoulder_contrast_in;
  if (use_hdr_in) {
    float hdr_extra_shoulder_power_factor = 2.0f;
    float hdr_shoulder_power_multiplier = pow(hdr_sdr_ratio,
                                              log10f(hdr_extra_shoulder_power_factor));
    shoulder_contrast_applied = shoulder_contrast_in * hdr_shoulder_power_multiplier;
  }

  // apply sigmoid, the image is formed at this point
  rgb.x = sigmoid(rgb.x,
                  shoulder_contrast_applied,
                  toe_contrast_in,
                  general_contrast,
                  log_midgray_applied,
                  display_native_midgray_applied);
  rgb.y = sigmoid(rgb.y,
                  shoulder_contrast_applied,
                  toe_contrast_in,
                  general_contrast,
                  log_midgray_applied,
                  display_native_midgray_applied);
  rgb.z = sigmoid(rgb.z,
                  shoulder_contrast_applied,
                  toe_contrast_in,
                  general_contrast,
                  log_midgray_applied,
                  display_native_midgray_applied);

  float3 img = rgb;
  // Linearize the formed image assuming its native transfer function is Rec.1886 curve
  img = spowf3(img, 2.4f);

  // HDR darken middle gray
  if (use_hdr_in) {
    float3 pre_darken_hsv;
    rgb_to_hsv_v(img, pre_darken_hsv);
    img = lin2log(img, 3, -20.0f, 2.47393118833f);

    float hdr_mg_pre_darken = lin2log(float3(0.18f, 0.18f, 0.18f), 3, -20.0f, 2.47393118833f).x;
    float hdr_mg_darkened =
        lin2log(float3(0.18f / hdr_sdr_ratio, 0.18f, 0.18f), 3, -20.0f, 2.47393118833f).x;
    // slope set to 1.000001 instead of 1.0 to prevent the curve from breaking when hdr_peak_in ==
    // hdr_midgray_factor_in
    img.x = sigmoid(img.x, 1, 3, 1.000001f, hdr_mg_pre_darken, hdr_mg_darkened, 1, 0);
    img.y = sigmoid(img.y, 1, 3, 1.000001f, hdr_mg_pre_darken, hdr_mg_darkened, 1, 0);
    img.z = sigmoid(img.z, 1, 3, 1.000001f, hdr_mg_pre_darken, hdr_mg_darkened, 1, 0);

    img = log2lin(img, 3, -20.0f, 2.47393118833f);

    float3 post_darken_hsv;
    rgb_to_hsv_v(img, post_darken_hsv);
    post_darken_hsv.x = lerp_chromaticity_angle(
        pre_darken_hsv.x, post_darken_hsv.x, hdr_purity_in);
    post_darken_hsv.y = pre_darken_hsv.y + hdr_purity_in * (post_darken_hsv.y - pre_darken_hsv.y);
    hsv_to_rgb_v(post_darken_hsv, img);
  }

  // lerp pre- and post-curve chromaticity angle
  float3 post_curve_hsv;
  rgb_to_hsv_v(img, post_curve_hsv);
  post_curve_hsv[0] = lerp_chromaticity_angle(
      pre_curve_hsv[0], post_curve_hsv[0], hue_flight_strength_in);
  hsv_to_rgb_v(post_curve_hsv, img);

  // check whether outset matrix is precomputed, if not, calculate it per-pixel
  float3x3 outsetmat_perpixel;
  if (std::isnan(outsetmat_precomputed[0][0])) {
    float3x3 outset_mat_inv_inset;
    float3x3 outset_mat;

    Chromaticities outset_inv_inset_chromaticities = InsetPrimaries(
        COLOR_SPACE_PRI[static_cast<int>(p_working_primaries)],
        attenuation_rates_in.x,
        attenuation_rates_in.y,
        attenuation_rates_in.z,
        rgb_rotation_in.x,
        rgb_rotation_in.y,
        rgb_rotation_in.z,
        /* Above parameters use pre-processing settings */
        tinting_hue_in + 3.14159265358979323846,
        tinting_scale_in);
    outset_mat_inv_inset = blender::math::invert(RGBtoRGB(
        outset_inv_inset_chromaticities, COLOR_SPACE_PRI[static_cast<int>(p_working_primaries)]));

    Chromaticities outset_chromaticities = InsetPrimaries(
        COLOR_SPACE_PRI[static_cast<int>(p_working_primaries)],
        restore_purity_in.x,
        restore_purity_in.y,
        restore_purity_in.z,
        reverse_rgb_rotation_in.x,
        reverse_rgb_rotation_in.y,
        reverse_rgb_rotation_in.z,
        tinting_hue_in + 3.14159265358979323846,
        tinting_scale_in);
    outset_mat = blender::math::invert(
        RGBtoRGB(outset_chromaticities, COLOR_SPACE_PRI[static_cast<int>(p_working_primaries)]));
    outsetmat_perpixel = p_use_inverse_inset ? outset_mat_inv_inset : outset_mat;
  }

  // apply outset matrix
  if (std::isnan(outsetmat_precomputed[0][0])) {
    img = outsetmat_perpixel * img;
  }
  else {
    img = outsetmat_precomputed * img;
  }

  // convert from working primaries to target display primaries
  img = working_to_display * img;

  // apply low-side guard rail if the UI checkbox is true, otherwise hard clamp to 0
  if (compensate_negatives_in) {
    img = compensate_low_side(img, true, display_to_rec2020);
  }
  else {
    img = maxf3(0, img);
  }

  img = minf3(1, img);

  float sdr_peak_assumption_for_edr_renormalization = 100.0f;
  if (use_hdr_in) {
    img *= (hdr_peak_in / sdr_peak_assumption_for_edr_renormalization);
  }

  // convert linearized formed image back to OCIO's scene_linear role space
  img = display_to_scene_linear * img;

  // re-combine the alpha channel
  float4 img_rgba;
  img_rgba.x = img.x;

  img_rgba.y = img.y;

  img_rgba.z = img.z;

  img_rgba.w = color.w;

  return img_rgba;
}

// GPU Processing
static int node_gpu_material(GPUMaterial *material,
                             bNode *node,
                             bNodeExecData * /*execdata*/,
                             GPUNodeStack *inputs,
                             GPUNodeStack *outputs)
{
  // ---- precompute maths that are the same for all pixels ----
  float linear_midgray_percentage_in = inputs[4].vec[0];
  float log2_min_in = inputs[5].vec[0];
  float log2_max_in = inputs[6].vec[0];

  float3 attenuation_rates_in = float3(inputs[9].vec[0], inputs[9].vec[1], inputs[9].vec[2]);
  float3 rgb_rotation_in = float3(inputs[8].vec[0], inputs[8].vec[1], inputs[8].vec[2]);

  float3 restore_purity_in = float3(inputs[11].vec[0], inputs[11].vec[1], inputs[11].vec[2]);
  float3 reverse_rgb_rotation_in = float3(inputs[10].vec[0], inputs[10].vec[1], inputs[10].vec[2]);
  float tinting_hue_in = inputs[14].vec[0];
  float tinting_scale_in = inputs[13].vec[0];
  bool p_use_inverse_inset = node->custom1;

  // find curve sockets
  bNodeSocket *log2_exposure_min_soc = const_cast<bNodeSocket *>(
      blender::bke::node_find_socket(*node, SOCK_IN, "Log2 Minimum Exposure"));
  bNodeSocket *log2_exposure_max_soc = const_cast<bNodeSocket *>(
      blender::bke::node_find_socket(*node, SOCK_IN, "Log2 Maximum Exposure"));
  bNodeSocket *linear_midgray_percent_soc = const_cast<bNodeSocket *>(
      blender::bke::node_find_socket(*node, SOCK_IN, "Middle Gray"));

  // find inset sockets
  bNodeSocket *attenuation_rates_soc = const_cast<bNodeSocket *>(
      blender::bke::node_find_socket(*node, SOCK_IN, "Purity Attenuation"));
  bNodeSocket *rgb_rotation_soc = const_cast<bNodeSocket *>(
      blender::bke::node_find_socket(*node, SOCK_IN, "RGB Rotation"));

  // find outset sockets
  bNodeSocket *reverse_rgb_rotation_soc = const_cast<bNodeSocket *>(
      blender::bke::node_find_socket(*node, SOCK_IN, "Reverse RGB Rotation"));
  bNodeSocket *restore_purity_soc = const_cast<bNodeSocket *>(
      blender::bke::node_find_socket(*node, SOCK_IN, "Restore Purity"));
  bNodeSocket *tinting_hue_soc = const_cast<bNodeSocket *>(
      blender::bke::node_find_socket(*node, SOCK_IN, "Tinting Hue"));
  bNodeSocket *tinting_scale_soc = const_cast<bNodeSocket *>(
      blender::bke::node_find_socket(*node, SOCK_IN, "Tinting Scale"));

  AgxPrecomputeData precomputed_data = agx_precompute(log2_min_in,
                                                      log2_max_in,
                                                      linear_midgray_percentage_in,
                                                      rgb_rotation_in,
                                                      attenuation_rates_in,
                                                      reverse_rgb_rotation_in,
                                                      restore_purity_in,
                                                      tinting_scale_in,
                                                      tinting_hue_in,
                                                      p_use_inverse_inset,
                                                      static_cast<int>(node->custom2),
                                                      static_cast<int>(node->custom3),
                                                      static_cast<int>(node->custom4));

  if (log2_exposure_min_soc->is_directly_linked() || log2_exposure_max_soc->is_directly_linked()) {
    precomputed_data.log_midgray = NAN;
  }

  if (linear_midgray_percent_soc->is_directly_linked()) {
    precomputed_data.display_native_midgray = NAN;
  }

  if (attenuation_rates_soc->is_directly_linked() || rgb_rotation_soc->is_directly_linked()) {
    if (p_use_inverse_inset) {
      precomputed_data.insetmat = float3x3({NAN, NAN, NAN}, {NAN, NAN, NAN}, {NAN, NAN, NAN});
      precomputed_data.outsetmat = float3x3({NAN, NAN, NAN}, {NAN, NAN, NAN}, {NAN, NAN, NAN});
    }
    else {
      precomputed_data.insetmat = float3x3({NAN, NAN, NAN}, {NAN, NAN, NAN}, {NAN, NAN, NAN});
    }
  }

  if (reverse_rgb_rotation_soc->is_directly_linked() || restore_purity_soc->is_directly_linked() ||
      tinting_scale_soc->is_directly_linked() || tinting_hue_soc->is_directly_linked())
  {
    precomputed_data.outsetmat = float3x3({NAN, NAN, NAN}, {NAN, NAN, NAN}, {NAN, NAN, NAN});
  }

  const float3x3 scene_linear_to_working = precomputed_data.scene_linear_to_working;
  const float3x3 working_to_display = precomputed_data.working_to_display;
  const float3x3 display_to_scene_linear = precomputed_data.display_to_scene_linear;
  const float log_midgray = precomputed_data.log_midgray;
  const float display_native_midgray = precomputed_data.display_native_midgray;
  const float3x3 insetmat = precomputed_data.insetmat;
  const float3x3 outsetmat = precomputed_data.outsetmat;
  const float3x3 working_to_rec2020 = precomputed_data.working_to_rec2020;
  const float3x3 display_to_rec2020 = precomputed_data.display_to_rec2020;
  float p_working_primaries = (float)node->custom2;
  float p_working_log = (float)node->custom3;
  float p_use_inverse_inset_float = (float)node->custom1;

  return GPU_stack_link(material,
                        node,
                        "node_composite_agx_view_transform",
                        inputs,
                        outputs,
                        GPU_uniform(&p_working_primaries),
                        GPU_uniform(&p_working_log),
                        GPU_uniform(&p_use_inverse_inset_float),
                        GPU_uniform(blender::float4x4(scene_linear_to_working).base_ptr()),
                        GPU_uniform(blender::float4x4(working_to_display).base_ptr()),
                        GPU_uniform(blender::float4x4(display_to_scene_linear).base_ptr()),
                        GPU_uniform(&log_midgray),
                        GPU_uniform(&display_native_midgray),
                        GPU_uniform(blender::float4x4(insetmat).base_ptr()),
                        GPU_uniform(blender::float4x4(outsetmat).base_ptr()),
                        GPU_uniform(blender::float4x4(working_to_rec2020).base_ptr()),
                        GPU_uniform(blender::float4x4(display_to_rec2020).base_ptr()));
}

// Multi Function
static void node_build_multi_function(blender::nodes::NodeMultiFunctionBuilder &builder)
{
  const bool use_inverse_inset = builder.node().custom1;
  const bool use_generic_log2 = builder.node().custom3 ==
                                int(AGXWorkingLog::AGX_WORKING_LOG_GENERIC_LOG2);

  // find curve sockets
  bNodeSocket *log2_exposure_min_soc = const_cast<bNodeSocket *>(
      blender::bke::node_find_socket(builder.node(), SOCK_IN, "Log2 Minimum Exposure"));
  bNodeSocket *log2_exposure_max_soc = const_cast<bNodeSocket *>(
      blender::bke::node_find_socket(builder.node(), SOCK_IN, "Log2 Maximum Exposure"));
  bNodeSocket *linear_midgray_percent_soc = const_cast<bNodeSocket *>(
      blender::bke::node_find_socket(builder.node(), SOCK_IN, "Middle Gray"));

  // find inset sockets
  bNodeSocket *attenuation_rates_soc = const_cast<bNodeSocket *>(
      blender::bke::node_find_socket(builder.node(), SOCK_IN, "Purity Attenuation"));
  bNodeSocket *rgb_rotation_soc = const_cast<bNodeSocket *>(
      blender::bke::node_find_socket(builder.node(), SOCK_IN, "RGB Rotation"));

  // find outset sockets
  bNodeSocket *reverse_rgb_rotation_soc = const_cast<bNodeSocket *>(
      blender::bke::node_find_socket(builder.node(), SOCK_IN, "Reverse RGB Rotation"));
  bNodeSocket *restore_purity_soc = const_cast<bNodeSocket *>(
      blender::bke::node_find_socket(builder.node(), SOCK_IN, "Restore Purity"));
  bNodeSocket *tinting_hue_soc = const_cast<bNodeSocket *>(
      blender::bke::node_find_socket(builder.node(), SOCK_IN, "Tinting Hue"));
  bNodeSocket *tinting_scale_soc = const_cast<bNodeSocket *>(
      blender::bke::node_find_socket(builder.node(), SOCK_IN, "Tinting Scale"));

  // get socket values
  float log2_min_in;
  if (log2_exposure_min_soc) {
    log2_min_in = node_socket_get_float(const_cast<bNodeTree *>(&builder.tree()),
                                        const_cast<bNode *>(&builder.node()),
                                        log2_exposure_min_soc);
  }

  float log2_max_in;
  if (log2_exposure_max_soc) {
    log2_max_in = node_socket_get_float(const_cast<bNodeTree *>(&builder.tree()),
                                        const_cast<bNode *>(&builder.node()),
                                        log2_exposure_max_soc);
  }

  float linear_midgray_percentage_in;
  if (linear_midgray_percent_soc) {
    linear_midgray_percentage_in = node_socket_get_float(const_cast<bNodeTree *>(&builder.tree()),
                                                         const_cast<bNode *>(&builder.node()),
                                                         linear_midgray_percent_soc);
  }

  float3 attenuation_rates_in;
  if (attenuation_rates_soc) {
    node_socket_get_vector(const_cast<bNodeTree *>(&builder.tree()),
                           const_cast<bNode *>(&builder.node()),
                           attenuation_rates_soc,
                           (float *)&attenuation_rates_in);
  }

  float3 rgb_rotation_in;
  if (rgb_rotation_soc) {
    node_socket_get_vector(const_cast<bNodeTree *>(&builder.tree()),
                           const_cast<bNode *>(&builder.node()),
                           rgb_rotation_soc,
                           (float *)&rgb_rotation_in);
  }

  float3 restore_purity_in;
  if (restore_purity_soc) {
    node_socket_get_vector(const_cast<bNodeTree *>(&builder.tree()),
                           const_cast<bNode *>(&builder.node()),
                           restore_purity_soc,
                           (float *)&restore_purity_in);
  }

  float3 reverse_rgb_rotation_in;
  if (reverse_rgb_rotation_soc) {
    node_socket_get_vector(const_cast<bNodeTree *>(&builder.tree()),
                           const_cast<bNode *>(&builder.node()),
                           reverse_rgb_rotation_soc,
                           (float *)&reverse_rgb_rotation_in);
  }

  float tinting_hue_in;
  if (tinting_hue_soc) {
    tinting_hue_in = node_socket_get_float(const_cast<bNodeTree *>(&builder.tree()),
                                           const_cast<bNode *>(&builder.node()),
                                           tinting_hue_soc);
  }

  float tinting_scale_in;
  if (tinting_scale_soc) {
    tinting_scale_in = node_socket_get_float(const_cast<bNodeTree *>(&builder.tree()),
                                             const_cast<bNode *>(&builder.node()),
                                             tinting_scale_soc);
  }

  // precompute
  AgxPrecomputeData precomputed_data = agx_precompute(log2_min_in,
                                                      log2_max_in,
                                                      linear_midgray_percentage_in,
                                                      rgb_rotation_in,
                                                      attenuation_rates_in,
                                                      reverse_rgb_rotation_in,
                                                      restore_purity_in,
                                                      tinting_scale_in,
                                                      tinting_hue_in,
                                                      use_inverse_inset,
                                                      static_cast<int>(builder.node().custom2),
                                                      static_cast<int>(builder.node().custom3),
                                                      static_cast<int>(builder.node().custom4));

  if (log2_exposure_min_soc->is_directly_linked() || log2_exposure_max_soc->is_directly_linked()) {
    precomputed_data.log_midgray = NAN;
  }

  if (linear_midgray_percent_soc->is_directly_linked()) {
    precomputed_data.display_native_midgray = NAN;
  }

  if (attenuation_rates_soc->is_directly_linked() || rgb_rotation_soc->is_directly_linked()) {
    if (use_inverse_inset) {
      precomputed_data.insetmat = float3x3({NAN, NAN, NAN}, {NAN, NAN, NAN}, {NAN, NAN, NAN});
      precomputed_data.outsetmat = float3x3({NAN, NAN, NAN}, {NAN, NAN, NAN}, {NAN, NAN, NAN});
    }
    else {
      precomputed_data.insetmat = float3x3({NAN, NAN, NAN}, {NAN, NAN, NAN}, {NAN, NAN, NAN});
    }
  }

  if (reverse_rgb_rotation_soc->is_directly_linked() || restore_purity_soc->is_directly_linked() ||
      tinting_scale_soc->is_directly_linked() || tinting_hue_soc->is_directly_linked())
  {
    precomputed_data.outsetmat = float3x3({NAN, NAN, NAN}, {NAN, NAN, NAN}, {NAN, NAN, NAN});
  }

  if (!use_inverse_inset && use_generic_log2) {
    builder.construct_and_set_matching_fn_cb([&]() {
      return mf::build::detail::build_multi_function_with_n_inputs_one_output<float4>(
          "AgX View Transform",
          [&builder, precomputed_data](const float4 &color,
                                       const float general_contrast_in,
                                       const float toe_contrast_in,
                                       const float shoulder_contrast_in,
                                       const float linear_midgray_percentage_in,
                                       const float log2_min_in,
                                       const float log2_max_in,
                                       const float log2_maintain_contrast_in,
                                       const float3 rgb_rotation_in,
                                       const float3 attenuation_rates_in,
                                       const float3 reverse_rgb_rotation_in,
                                       const float3 restore_purity_in,
                                       const float hue_flight_strength_in,
                                       const float tinting_scale_in,
                                       const float tinting_hue_in,
                                       const bool compensate_negatives_in,
                                       const bool use_hdr_in,
                                       const float hdr_peak_in,
                                       const float hdr_midgray_factor_in,
                                       const float hdr_purity_in) -> float4 {
            return agx_image_formation(color,
                                       general_contrast_in,
                                       toe_contrast_in,
                                       shoulder_contrast_in,
                                       linear_midgray_percentage_in,
                                       log2_min_in,
                                       log2_max_in,
                                       log2_maintain_contrast_in,
                                       rgb_rotation_in,
                                       attenuation_rates_in,
                                       reverse_rgb_rotation_in,
                                       restore_purity_in,
                                       hue_flight_strength_in,
                                       tinting_scale_in,
                                       tinting_hue_in,
                                       compensate_negatives_in,
                                       use_hdr_in,
                                       hdr_peak_in,
                                       hdr_midgray_factor_in,
                                       hdr_purity_in,
                                       static_cast<int>(builder.node().custom2),
                                       static_cast<int>(builder.node().custom3),
                                       static_cast<bool>(builder.node().custom1),
                                       precomputed_data.scene_linear_to_working,
                                       precomputed_data.working_to_display,
                                       precomputed_data.display_to_scene_linear,
                                       precomputed_data.log_midgray,
                                       precomputed_data.display_native_midgray,
                                       precomputed_data.insetmat,
                                       precomputed_data.outsetmat,
                                       precomputed_data.working_to_rec2020,
                                       precomputed_data.display_to_rec2020);
          },
          mf::build::exec_presets::SomeSpanOrSingle<0>(),
          TypeSequence<float4,
                       float,
                       float,
                       float,
                       float,
                       float,
                       float,
                       float,
                       float3,
                       float3,
                       float3,
                       float3,
                       float,
                       float,
                       float,
                       bool,
                       bool,
                       float,
                       float,
                       float>());
    });
  }
  else if (use_inverse_inset && use_generic_log2) {
    builder.construct_and_set_matching_fn_cb([&]() {
      return mf::build::detail::build_multi_function_with_n_inputs_one_output<float4>(
          "AgX View Transform",
          [&builder, precomputed_data](const float4 &color,
                                       const float general_contrast_in,
                                       const float toe_contrast_in,
                                       const float shoulder_contrast_in,
                                       const float linear_midgray_percentage_in,
                                       const float log2_min_in,
                                       const float log2_max_in,
                                       const float log2_maintain_contrast_in,
                                       const float3 rgb_rotation_in,
                                       const float3 attenuation_rates_in,
                                       const float hue_flight_strength_in,
                                       const float tinting_scale_in,
                                       const float tinting_hue_in,
                                       const bool compensate_negatives_in,
                                       const bool use_hdr_in,
                                       const float hdr_peak_in,
                                       const float hdr_midgray_factor_in,
                                       const float hdr_purity_in) -> float4 {
            return agx_image_formation(color,
                                       general_contrast_in,
                                       toe_contrast_in,
                                       shoulder_contrast_in,
                                       linear_midgray_percentage_in,
                                       log2_min_in,
                                       log2_max_in,
                                       log2_maintain_contrast_in,
                                       rgb_rotation_in,
                                       attenuation_rates_in,
                                       float3(0.0f, 0.0f, 0.0f),  // reverse_rgb_rotation_in,
                                       float3(0.0f, 0.0f, 0.0f),  // restore_purity_in,
                                       hue_flight_strength_in,
                                       tinting_scale_in,
                                       tinting_hue_in,
                                       compensate_negatives_in,
                                       use_hdr_in,
                                       hdr_peak_in,
                                       hdr_midgray_factor_in,
                                       hdr_purity_in,
                                       static_cast<int>(builder.node().custom2),
                                       static_cast<int>(builder.node().custom3),
                                       static_cast<bool>(builder.node().custom1),
                                       precomputed_data.scene_linear_to_working,
                                       precomputed_data.working_to_display,
                                       precomputed_data.display_to_scene_linear,
                                       precomputed_data.log_midgray,
                                       precomputed_data.display_native_midgray,
                                       precomputed_data.insetmat,
                                       precomputed_data.outsetmat,
                                       precomputed_data.working_to_rec2020,
                                       precomputed_data.display_to_rec2020);
          },
          mf::build::exec_presets::SomeSpanOrSingle<0>(),
          TypeSequence<float4,
                       float,
                       float,
                       float,
                       float,
                       float,
                       float,
                       float,
                       float3,
                       float3,
                       float,
                       float,
                       float,
                       bool,
                       bool,
                       float,
                       float,
                       float>());
    });
  }
  else if (!use_inverse_inset && !use_generic_log2) {
    builder.construct_and_set_matching_fn_cb([&]() {
      return mf::build::detail::build_multi_function_with_n_inputs_one_output<float4>(
          "AgX View Transform",
          [&builder, precomputed_data](const float4 &color,
                                       const float general_contrast_in,
                                       const float toe_contrast_in,
                                       const float shoulder_contrast_in,
                                       const float linear_midgray_percentage_in,
                                       const float3 rgb_rotation_in,
                                       const float3 attenuation_rates_in,
                                       const float3 reverse_rgb_rotation_in,
                                       const float3 restore_purity_in,
                                       const float hue_flight_strength_in,
                                       const float tinting_scale_in,
                                       const float tinting_hue_in,
                                       const bool compensate_negatives_in,
                                       const bool use_hdr_in,
                                       const float hdr_peak_in,
                                       const float hdr_midgray_factor_in,
                                       const float hdr_purity_in) -> float4 {
            return agx_image_formation(color,
                                       general_contrast_in,
                                       toe_contrast_in,
                                       shoulder_contrast_in,
                                       linear_midgray_percentage_in,
                                       -10.0f, /* log2_min_in */
                                       6.5f,   /* log2_max_in */
                                       0,      /* log2_maintain_contrast_in */
                                       rgb_rotation_in,
                                       attenuation_rates_in,
                                       reverse_rgb_rotation_in,
                                       restore_purity_in,
                                       hue_flight_strength_in,
                                       tinting_scale_in,
                                       tinting_hue_in,
                                       compensate_negatives_in,
                                       use_hdr_in,
                                       hdr_peak_in,
                                       hdr_midgray_factor_in,
                                       hdr_purity_in,
                                       static_cast<int>(builder.node().custom2),
                                       static_cast<int>(builder.node().custom3),
                                       static_cast<bool>(builder.node().custom1),
                                       precomputed_data.scene_linear_to_working,
                                       precomputed_data.working_to_display,
                                       precomputed_data.display_to_scene_linear,
                                       precomputed_data.log_midgray,
                                       precomputed_data.display_native_midgray,
                                       precomputed_data.insetmat,
                                       precomputed_data.outsetmat,
                                       precomputed_data.working_to_rec2020,
                                       precomputed_data.display_to_rec2020);
          },
          mf::build::exec_presets::SomeSpanOrSingle<0>(),
          TypeSequence<float4,
                       float,
                       float,
                       float,
                       float,
                       float3,
                       float3,
                       float3,
                       float3,
                       float,
                       float,
                       float,
                       bool,
                       bool,
                       float,
                       float,
                       float>());
    });
  }
  else { /* use_inverse_inset && !use_generic_log2 */
    builder.construct_and_set_matching_fn_cb([&]() {
      return mf::build::detail::build_multi_function_with_n_inputs_one_output<float4>(
          "AgX View Transform",
          [&builder, precomputed_data](const float4 &color,
                                       const float general_contrast_in,
                                       const float toe_contrast_in,
                                       const float shoulder_contrast_in,
                                       const float linear_midgray_percentage_in,
                                       const float3 rgb_rotation_in,
                                       const float3 attenuation_rates_in,
                                       const float hue_flight_strength_in,
                                       const float tinting_scale_in,
                                       const float tinting_hue_in,
                                       const bool compensate_negatives_in,
                                       const bool use_hdr_in,
                                       const float hdr_peak_in,
                                       const float hdr_midgray_factor_in,
                                       const float hdr_purity_in) -> float4 {
            return agx_image_formation(color,
                                       general_contrast_in,
                                       toe_contrast_in,
                                       shoulder_contrast_in,
                                       linear_midgray_percentage_in,
                                       -10.0f, /* log2_min_in */
                                       6.5f,   /* log2_max_in */
                                       0,      /* log2_maintain_contrast_in */
                                       rgb_rotation_in,
                                       attenuation_rates_in,
                                       float3(0.0f, 0.0f, 0.0f),  // reverse_rgb_rotation_in,
                                       float3(0.0f, 0.0f, 0.0f),  // restore_purity_in,
                                       hue_flight_strength_in,
                                       tinting_scale_in,
                                       tinting_hue_in,
                                       compensate_negatives_in,
                                       use_hdr_in,
                                       hdr_peak_in,
                                       hdr_midgray_factor_in,
                                       hdr_purity_in,
                                       static_cast<int>(builder.node().custom2),
                                       static_cast<int>(builder.node().custom3),
                                       static_cast<bool>(builder.node().custom1),
                                       precomputed_data.scene_linear_to_working,
                                       precomputed_data.working_to_display,
                                       precomputed_data.display_to_scene_linear,
                                       precomputed_data.log_midgray,
                                       precomputed_data.display_native_midgray,
                                       precomputed_data.insetmat,
                                       precomputed_data.outsetmat,
                                       precomputed_data.working_to_rec2020,
                                       precomputed_data.display_to_rec2020);
          },
          mf::build::exec_presets::SomeSpanOrSingle<0>(),
          TypeSequence<float4,
                       float,
                       float,
                       float,
                       float,
                       float3,
                       float3,
                       float,
                       float,
                       float,
                       bool,
                       bool,
                       float,
                       float,
                       float>());
    });
  }
}

// Registration Function
static void node_register()
{
  static blender::bke::bNodeType ntype;

  cmp_node_type_base(&ntype, "CompositorNodeAgXViewTransform");
  ntype.ui_name = "AgX View Transform";
  ntype.ui_description =
      "Apply AgX Picture Formation that converts rendered RGB exposure into an Image for Display";
  ntype.enum_name_legacy = "AGX_VIEW_TRANSFORM";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = node_declare;
  ntype.updatefunc = node_update;
  ntype.initfunc = node_init;
  blender::bke::node_type_size(ntype, 180, 150, 240);
  ntype.build_multi_function = node_build_multi_function;
  ntype.gpu_fn = node_gpu_material;
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_composite_agx_view_transform_cc
