/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_common_color_utils.glsl"
#include "gpu_shader_math_base_lib.glsl"
#include "gpu_shader_math_matrix_lib.glsl"
#include "gpu_shader_math_vector_lib.glsl"

// A struct holding xy for R, G, B, and White point.
struct Chromaticities {
  float2 red;
  float2 green;
  float2 blue;
  float2 white;
};

float4 spowf4(float4 a, float b)
{
  return float4(sign(a.x) * pow(abs(a.x), b),
                sign(a.y) * pow(abs(a.y), b),
                sign(a.z) * pow(abs(a.z), b),
                a.w);
}

float spowf(float a, float b)
{
  return sign(a) * pow(abs(a), b);
}

float4 log2lin(float4 rgba,
               int tf,
               float generic_log2_min_expo = -10,
               float generic_log2_max_expo = 6.5)
{
  float3 rgb = rgba.rgb;
  if (tf == 0) {  // Arri LogC3 EI 800
    rgb.x = rgb.x > 0.149658f ?
                (pow(10.0f, (rgb.x - 0.385537f) / 0.24719f) - 0.052272f) / 5.555556f :
                (rgb.x - 0.092809f) / 5.367655f;
    rgb.y = rgb.y > 0.149658f ?
                (pow(10.0f, (rgb.y - 0.385537f) / 0.24719f) - 0.052272f) / 5.555556f :
                (rgb.y - 0.092809f) / 5.367655f;
    rgb.z = rgb.z > 0.149658f ?
                (pow(10.0f, (rgb.z - 0.385537f) / 0.24719f) - 0.052272f) / 5.555556f :
                (rgb.z - 0.092809f) / 5.367655f;
  }
  else if (tf == 1) {  // User controlled PureLog2
    float mx = generic_log2_max_expo;
    float mn = generic_log2_min_expo;

    rgb.x = 0.18 * pow(2, (rgb.x * (mx - mn) + mn));
    rgb.y = 0.18 * pow(2, (rgb.y * (mx - mn) + mn));
    rgb.z = 0.18 * pow(2, (rgb.z * (mx - mn) + mn));
  }
  return float4(rgb, rgba.a);
}

float4 lin2log(float4 rgba, int tf, float generic_log2_min_expo, float generic_log2_max_expo)
{
  float3 rgb = rgba.rgb;
  float log_floor = log2lin(float4(0.0f), tf, generic_log2_min_expo, generic_log2_max_expo).x;
  rgb = max(float3(log_floor), rgb);
  if (tf == 0) {  // Arri LogC3 EI 800
    rgb.x = rgb.x > 0.010591f ?
                0.24719f * (log(5.555556f * rgb.x + 0.052272f) / log(10.0f)) + 0.385537f :
                5.367655f * rgb.x + 0.092809f;
    rgb.y = rgb.y > 0.010591f ?
                0.24719f * (log(5.555556f * rgb.y + 0.052272f) / log(10.0f)) + 0.385537f :
                5.367655f * rgb.y + 0.092809f;
    rgb.z = rgb.z > 0.010591f ?
                0.24719f * (log(5.555556f * rgb.z + 0.052272f) / log(10.0f)) + 0.385537f :
                5.367655f * rgb.z + 0.092809f;
  }
  else if (tf == 1) {  // User controlled PureLog2
    rgb = log2(rgb / 0.18f);
    rgb = clamp(rgb, generic_log2_min_expo, generic_log2_max_expo);

    rgb = (rgb + abs(generic_log2_min_expo)) /
          (abs(generic_log2_min_expo) + abs(generic_log2_max_expo));
  }
  return float4(rgb, rgba.a);
}

float sigmoid(
    float in_val, float sp, float tp, float Pslope, float px, float py, float s0, float t0)
{
  // calculate Shoulder;
  float ss = spowf(
      ((spowf((Pslope * ((s0 - px) / (1 - py))), sp) - 1) * (spowf(Pslope * (s0 - px), -sp))),
      -1 / sp);
  float ms = Pslope * (in_val - px) / ss;
  float fs = ms / spowf(1 + (spowf(ms, sp)), 1 / sp);

  // calculate Toe
  float ts = spowf(
      ((spowf((Pslope * ((px - t0) / (py))), tp) - 1) * (spowf(Pslope * (px - t0), -tp))),
      -1 / tp);
  float mr = (Pslope * (in_val - px)) / -ts;
  float ft = mr / spowf(1 + (spowf(mr, tp)), 1 / tp);

  in_val = in_val >= px ? ss * fs + py : (-ts * ft) + py;

  return in_val;
}

float lerp_chromaticity_angle(float h1, float h2, float t)
{
  float delta = h2 - h1;
  if (delta > 0.5f)
    delta -= 1.0f;
  else if (delta < -0.5f)
    delta += 1.0f;
  float lerped = h1 + t * delta;
  return lerped - floor(lerped);
}

float4 compensate_low_side(float4 rgba, bool use_hacky_lerp, float4x4 input_pri_to_rec2020_mat)
{
  float3 rgb = rgba.rgb;
  // Hardcoded Rec.2020 luminance coefficients (2015 CMFs)
  const float3 luminance_coeffs = float3(
      0.2589235355689848f, 0.6104985346066525f, 0.13057792982436284f);

  // Convert RGB to Rec.2020 for luminance calculation
  float3 rgb_rec2020 = (input_pri_to_rec2020_mat * float4(rgb, 1.0)).rgb;

  // Calculate original luminance Y
  float Y = rgb_rec2020.x * luminance_coeffs.x + rgb_rec2020.y * luminance_coeffs.y +
            rgb_rec2020.z * luminance_coeffs.z;

  // Calculate inverse RGB in working space
  float max_rgb = max(rgb.x, max(rgb.y, rgb.z));
  float3 inverse_rgb = float3(max_rgb - rgb.x, max_rgb - rgb.y, max_rgb - rgb.z);

  // Calculate max of the inverse
  float max_inv_rgb = max(inverse_rgb.x, max(inverse_rgb.y, inverse_rgb.z));

  // Convert inverse RGB to Rec.2020 for Y calculation
  float3 inverse_rec2020 = (input_pri_to_rec2020_mat * float4(inverse_rgb, 1.0)).rgb;
  float Y_inverse = inverse_rec2020.x * luminance_coeffs.x +
                    inverse_rec2020.y * luminance_coeffs.y +
                    inverse_rec2020.z * luminance_coeffs.z;

  // Calculate compensation values
  float y_compensate = (max_inv_rgb - Y_inverse + Y);
  if (use_hacky_lerp) {
    float Y_clipped = clamp(pow(Y, 0.08f), 0.0f, 1.0f);
    y_compensate = y_compensate + Y_clipped * (Y - y_compensate);
  }

  // Offset to avoid negatives
  float min_rgb = min(rgb.x, min(rgb.y, rgb.z));
  float offset = max(-min_rgb, 0.0f);
  float3 rgb_offset = float3(rgb.x + offset, rgb.y + offset, rgb.z + offset);

  // Calculate max of the offseted RGB
  float max_offset = max(rgb_offset.x, max(rgb_offset.y, rgb_offset.z));

  // Calculate new luminance after offset
  float3 offset_rec2020 = (input_pri_to_rec2020_mat * float4(rgb_offset, 1.0)).rgb;
  float Y_new = offset_rec2020.x * luminance_coeffs.x + offset_rec2020.y * luminance_coeffs.y +
                offset_rec2020.z * luminance_coeffs.z;

  // Calculate the inverted RGB offset
  float3 inverse_offset = float3(
      max_offset - rgb_offset.x, max_offset - rgb_offset.y, max_offset - rgb_offset.z);

  // Calculate max of the inverse
  float max_inv_offset = max(inverse_offset.x, max(inverse_offset.y, inverse_offset.z));

  float3 inverse_offset_rec2020 = (input_pri_to_rec2020_mat * float4(inverse_offset, 1.0)).rgb;
  float Y_inverse_offset = inverse_offset_rec2020.x * luminance_coeffs.x +
                           inverse_offset_rec2020.y * luminance_coeffs.y +
                           inverse_offset_rec2020.z * luminance_coeffs.z;

  float Y_new_compensate = (max_inv_offset - Y_inverse_offset + Y_new);
  if (use_hacky_lerp) {
    float Y_new_clipped = clamp(pow(Y_new, 0.08f), 0.0f, 1.0f);
    Y_new_compensate = Y_new_compensate + Y_new_clipped * (Y_new - Y_new_compensate);
  }

  // Adjust luminance ratio
  float ratio = (Y_new_compensate > y_compensate) ? (y_compensate / Y_new_compensate) : 1.0f;
  float4 result_output = float4(
      rgb_offset.x * ratio, rgb_offset.y * ratio, rgb_offset.z * ratio, rgba.a);
  // to prevent small negative values close to zero from still occurring due to precision issues,
  // clamp to 0.0
  result_output = max(float4(0.0), result_output);
  return result_output;
}

float4x4 RGBtoXYZ(Chromaticities N)
{
  float3x3 M = float3x3(
      float3(N.red.x / N.red.y, 1.0, (1 - N.red.x - N.red.y) / N.red.y),
      float3(N.green.x / N.green.y, 1.0, (1 - N.green.x - N.green.y) / N.green.y),
      float3(N.blue.x / N.blue.y, 1.0, (1 - N.blue.x - N.blue.y) / N.blue.y));
  float3 wh = float3(N.white.x / N.white.y, 1.0, (1 - N.white.x - N.white.y) / N.white.y);
  wh = invert(M) * wh;
  M = float3x3(M[0] * wh.x, M[1] * wh.y, M[2] * wh.z);
  return to_float4x4(M);
}

float4x4 XYZtoRGB(Chromaticities N)
{
  float4x4 M = invert(RGBtoXYZ(N));
  return M;
}

float4x4 RGBtoRGB(Chromaticities N, Chromaticities M)
{
  float4x4 In2XYZ = RGBtoXYZ(N);
  float4x4 XYZ2Out = XYZtoRGB(M);

  float4x4 rgbtorgb = XYZ2Out * In2XYZ;

  return rgbtorgb;
}

Chromaticities CenterPrimaries(Chromaticities N)
{
  N.red.x = N.red.x - N.white.x;
  N.red.y = N.red.y - N.white.y;
  N.green.x = N.green.x - N.white.x;
  N.green.y = N.green.y - N.white.y;
  N.blue.x = N.blue.x - N.white.x;
  N.blue.y = N.blue.y - N.white.y;

  return N;
}

Chromaticities DeCenterPrimaries(Chromaticities N)
{
  N.red.x = N.red.x + N.white.x;
  N.red.y = N.red.y + N.white.y;
  N.green.x = N.green.x + N.white.x;
  N.green.y = N.green.y + N.white.y;
  N.blue.x = N.blue.x + N.white.x;
  N.blue.y = N.blue.y + N.white.y;

  return N;
}

float2 cartesian_to_polar2(float2 a)
{
  float2 b = a;
  b.y = atan2(a.y, a.x);

  return float2(sqrt(a.x * a.x + a.y * a.y), b.y);
}

float2 polar_to_cartesian2(float2 a)
{

  return float2(a.x * cos(a.y), a.x * sin(a.y));
}

Chromaticities RotatePrimary(Chromaticities N, float rrot, float grot, float brot)
{
  N = CenterPrimaries(N);
  N.red = cartesian_to_polar2(N.red);
  N.green = cartesian_to_polar2(N.green);
  N.blue = cartesian_to_polar2(N.blue);

  N.red.y = N.red.y + rrot;
  N.green.y = N.green.y + grot;
  N.blue.y = N.blue.y + brot;

  N.red = polar_to_cartesian2(N.red);
  N.green = polar_to_cartesian2(N.green);
  N.blue = polar_to_cartesian2(N.blue);

  N = DeCenterPrimaries(N);

  return N;
}

Chromaticities ScalePrim(Chromaticities N, float rs, float gs, float bs)
{
  N = CenterPrimaries(N);

  N.red = float2(N.red.x * rs, N.red.y * rs);
  N.green = float2(N.green.x * gs, N.green.y * gs);
  N.blue = float2(N.blue.x * bs, N.blue.y * bs);

  N = DeCenterPrimaries(N);

  return N;
}

float2 Line_equation(float2 a, float2 b)
{
  float dx = b.x - a.x;
  if (abs(dx) < 1e-6f) {
    /* vertical line → slope = +∞, intercept = x */
    const float kInf = 1e30f * 1e30f;
    return float2(kInf, a.x);
  }
  float m = (b.y - a.y) / dx;
  float c = a.y - m * a.x;
  return float2(m, c);
}

Chromaticities Polygon(Chromaticities N)
{
  Chromaticities M = N;

  N.red = Line_equation(M.red, M.green);
  N.green = Line_equation(M.red, M.blue);
  N.blue = Line_equation(M.blue, M.green);

  return N;
}

float2 intersection(float2 l1, float2 l2)
{
  float m1 = l1.x, c1 = l1.y;
  float m2 = l2.x, c2 = l2.y;

  bool inf1 = isinf(m1);
  bool inf2 = isinf(m2);

  float x, y;
  if (inf1 && inf2) {
    // two verticals (parallel) → just default (0,0)
    x = y = 0.0f;
  }
  else if (inf1) {
    // first is vertical: x = c1
    x = c1;
    y = m2 * x + c2;
  }
  else if (inf2) {
    // second is vertical: x = c2
    x = c2;
    y = m1 * x + c1;
  }
  else {
    x = (c2 - c1) / (m1 - m2);
    y = m1 * x + c1;
  }
  return float2(x, y);
}

bool on_segment(float2 P, float2 C, float2 D, float2 A, float2 B)
{
  float t = (abs(B.x - A.x) > abs(B.y - A.y)) ? (P.x - A.x) / (B.x - A.x) :
                                                (P.y - A.y) / (B.y - A.y);
  float u = (abs(D.x - C.x) > abs(D.y - C.y)) ? (P.x - C.x) / (D.x - C.x) :
                                                (P.y - C.y) / (D.y - C.y);
  return (t >= 0.0f && t <= 1.0f && u >= 0.0f && u <= 1.0f);
}

Chromaticities InsetPrimaries(Chromaticities N,
                              float red_scale,
                              float green_scale,
                              float blue_scale,
                              float red_rotate,
                              float green_rotate,
                              float blue_rotate,
                              float achromatic_rotate = 0,
                              float achromatic_outset = 0)
{
  Chromaticities M = N;

  Chromaticities original_N = N;
  Chromaticities scaled_N = ScalePrim(N, 4.0, 4.0, 4.0);

  N = RotatePrimary(scaled_N, red_rotate, green_rotate, blue_rotate);

  M = Polygon(M);

  float2 redline = red_rotate > 0 ? M.red : M.green;
  float2 greenline = green_rotate > 0 ? M.blue : M.red;
  float2 blueline = blue_rotate > 0 ? M.green : M.blue;

  // compute the line eqns from each rotated‐primary → whitepoint
  float2 wp = original_N.white;
  float2 lr = Line_equation(float2(N.red.x, N.red.y), wp);
  float2 lg = Line_equation(float2(N.green.x, N.green.y), wp);
  float2 lb = Line_equation(float2(N.blue.x, N.blue.y), wp);

  // intersect each with the chosen triangle edge
  float2 Pr = intersection(lr, redline);
  float2 Pg = intersection(lg, greenline);
  float2 Pb = intersection(lb, blueline);

  // assign back into N
  N.red = Pr;
  N.green = Pg;
  N.blue = Pb;

  red_scale = (100 - red_scale) / 100;
  green_scale = (100 - green_scale) / 100;
  blue_scale = (100 - blue_scale) / 100;

  N = ScalePrim(N, red_scale, green_scale, blue_scale);

  // --- Start of Achromatic Tinting ---
  Chromaticities poly_original_N = Polygon(original_N);
  float2 original_white = poly_original_N.white;
  const float arbitrary_scale = 4.0f;

  // Scale & rotate the achromatic point
  float2 scaled_achromatic = float2(original_white.x, original_white.y * arbitrary_scale);
  float dx = scaled_achromatic.x - original_white.x;
  float dy = scaled_achromatic.y - original_white.y;
  float2 rotated_achromatic = float2(
      original_white.x + dx * cos(achromatic_rotate) - dy * sin(achromatic_rotate),
      original_white.y + dx * sin(achromatic_rotate) + dy * cos(achromatic_rotate));

  // Build the infinite achromatic ray and triangle edges
  float2 la = Line_equation(rotated_achromatic, original_white);
  float2 e1 = Line_equation(original_N.red, original_N.green);
  float2 e2 = Line_equation(original_N.green, original_N.blue);
  float2 e3 = Line_equation(original_N.blue, original_N.red);

  // Find their intersections
  float2 i1 = intersection(la, e1);
  float2 i2 = intersection(la, e2);
  float2 i3 = intersection(la, e3);

  // Pick the first intersect that lies on both the achromatic ray segment (t∈[0,1])
  // and the edge segment (u∈[0,1]), else fallback to white
  float2 hull_achromatic = original_white;
  float2 A = rotated_achromatic, B = original_white;

  if (on_segment(i1, original_N.red, original_N.green, A, B))
    hull_achromatic = i1;
  else if (on_segment(i2, original_N.green, original_N.blue, A, B))
    hull_achromatic = i2;
  else if (on_segment(i3, original_N.blue, original_N.red, A, B))
    hull_achromatic = i3;

  // Move whitepoint towards hull_achromatic by achromatic_outset
  float2 interp = float2((original_white.x - hull_achromatic.x) * (1.0f - achromatic_outset),
                         (original_white.y - hull_achromatic.y) * (1.0f - achromatic_outset));
  N.white.x = hull_achromatic.x + interp.x;
  N.white.y = hull_achromatic.y + interp.y;

  return N;
}

void node_composite_agx_view_transform(float4 color,
                                       float general_contrast_in,
                                       float toe_contrast_in,
                                       float shoulder_contrast_in,
                                       float linear_midgray_percentage_in,
                                       float log2_min_in,
                                       float log2_max_in,
                                       float log2_maintain_contrast_in,
                                       float3 rgb_rotation_in,
                                       float3 attenuation_rates_in,
                                       float3 reverse_rgb_rotation_in,
                                       float3 restore_purity_in,
                                       float hue_flight_strength_in,
                                       float tinting_scale_in,
                                       float tinting_hue_in,
                                       float compensate_negatives_in,
                                       float use_hdr_in,
                                       float hdr_peak_in,
                                       float hdr_midgray_factor_in,
                                       float hdr_purity_in,
                                       float p_working_primaries,
                                       float p_working_log,
                                       float p_use_inverse_inset,
                                       float4x4 scene_linear_to_working,
                                       float4x4 working_to_display,
                                       float4x4 display_to_scene_linear,
                                       float log_midgray_precomputed,
                                       float display_native_midgray_precomputed,
                                       float4x4 insetmat_precomputed,
                                       float4x4 outsetmat_precomputed,
                                       float4x4 working_to_rec2020,
                                       float4x4 display_to_rec2020,
                                       out float4 result)
{
  // Color Spaces Coordinates
  const Chromaticities P3D65_PRI = Chromaticities(
      /* r: */ float2(0.68f, 0.32f),
      /* g: */ float2(0.265f, 0.69f),
      /* b: */ float2(0.15f, 0.06f),
      /* w: */ float2(0.3127f, 0.3290f));

  const Chromaticities REC709_PRI = Chromaticities(
      /* r: */ float2(0.64f, 0.33f),
      /* g: */ float2(0.3f, 0.6f),
      /* b: */ float2(0.15f, 0.06f),
      /* w: */ float2(0.3127f, 0.3290f));

  const Chromaticities REC2020_PRI = Chromaticities(
      /* r: */ float2(0.708f, 0.292f),
      /* g: */ float2(0.17f, 0.797f),
      /* b: */ float2(0.131f, 0.046f),
      /* w: */ float2(0.3127f, 0.3290f));

  const Chromaticities AWG3_PRI = Chromaticities(
      /* r: */ float2(0.684f, 0.313f),
      /* g: */ float2(0.221f, 0.848f),
      /* b: */ float2(0.0861f, -0.102f),
      /* w: */ float2(0.3127f, 0.3290f));

  const Chromaticities COLOR_SPACE_PRI[] = {
      P3D65_PRI, REC709_PRI, REC2020_PRI, AWG3_PRI};

  color = scene_linear_to_working * color;

  // apply low-side guard rail if the UI checkbox is true, otherwise hard clamp to 0
  if (bool(compensate_negatives_in)) {
    color = compensate_low_side(color, false, working_to_rec2020);
  }
  else {
    color = max(float4(0.0), color);
  }

  // check whether inset matrix is precomputed, if not, calculate it per-pixel
  float4x4 insetmat_perpixel;
  if (isnan(insetmat_precomputed[0][0])) {
    Chromaticities inset_chromaticities = InsetPrimaries(COLOR_SPACE_PRI[int(p_working_primaries)],
                                                         attenuation_rates_in.x,
                                                         attenuation_rates_in.y,
                                                         attenuation_rates_in.z,
                                                         rgb_rotation_in.x,
                                                         rgb_rotation_in.y,
                                                         rgb_rotation_in.z);
    insetmat_perpixel = RGBtoRGB(inset_chromaticities, COLOR_SPACE_PRI[int(p_working_primaries)]);
  }

  // apply inset matrix
  if (isnan(insetmat_precomputed[0][0])) {
    color = insetmat_perpixel * color;
  }
  else {
    color = insetmat_precomputed * color;
  }
  // record pre-formation chromaticity angle
  float4 pre_curve_hsv;
  rgb_to_hsv(color, pre_curve_hsv);

  // encode to working log
  color = lin2log(color, int(p_working_log), log2_min_in, log2_max_in);

  if (bool(log2_maintain_contrast_in) && int(p_working_log) == 3) {
    float base_exposure_range = 16.5f;
    general_contrast_in = ((abs(log2_min_in) + log2_max_in) / base_exposure_range) *
                          general_contrast_in;
  }

  // check whether log middle gray is precomputed, if no, calculate it per-pixel.
  float log_midgray_perpixel;
  if (isnan(log_midgray_precomputed)) {
    log_midgray_perpixel =
        lin2log(float4(0.18f, 0.18f, 0.18f, 1.0f), int(p_working_log), log2_min_in, log2_max_in).x;
  }

  // check whether display native middle gray is precomputed, if no, calculate it per-pixel.
  float display_native_midgray_perpixel;
  if (isnan(display_native_midgray_precomputed)) {
    float display_native_power = 2.4f;
    float lin_midgray_float = linear_midgray_percentage_in / 100.0f;
    display_native_midgray_perpixel = pow(lin_midgray_float, 1.0f / display_native_power);
  }

  float log_midgray_applied;
  float display_native_midgray_applied;

  if (isnan(log_midgray_precomputed)) {
    log_midgray_applied = log_midgray_perpixel;
  }
  else {
    log_midgray_applied = log_midgray_precomputed;
  }

  if (isnan(display_native_midgray_precomputed)) {
    display_native_midgray_applied = display_native_midgray_perpixel;
  }
  else {
    display_native_midgray_applied = display_native_midgray_precomputed;
  }

  float hdr_sdr_ratio = hdr_peak_in / hdr_midgray_factor_in;
  float shoulder_contrast_applied = shoulder_contrast_in;
  if (bool(use_hdr_in)) {
    float hdr_extra_shoulder_power_factor = 2.0f;
    float hdr_shoulder_power_multiplier = pow(hdr_sdr_ratio,
                                              log(hdr_extra_shoulder_power_factor) / log(10.0f));
    shoulder_contrast_applied = shoulder_contrast_in * hdr_shoulder_power_multiplier;
  }

  // apply sigmoid, the image is formed at this point
  color.x = sigmoid(color.x,
                    shoulder_contrast_applied,
                    toe_contrast_in,
                    general_contrast_in,
                    log_midgray_applied,
                    display_native_midgray_applied,
                    1.0f,
                    0.0f);
  color.y = sigmoid(color.y,
                    shoulder_contrast_applied,
                    toe_contrast_in,
                    general_contrast_in,
                    log_midgray_applied,
                    display_native_midgray_applied,
                    1.0f,
                    0.0f);
  color.z = sigmoid(color.z,
                    shoulder_contrast_applied,
                    toe_contrast_in,
                    general_contrast_in,
                    log_midgray_applied,
                    display_native_midgray_applied,
                    1.0f,
                    0.0f);

  float4 img = color;
  // Linearize the formed image assuming its native transfer function is Rec.1886 curve
  img = spowf4(img, 2.4f);

  if (bool(use_hdr_in)) {
    float4 pre_darken_hsv;
    rgb_to_hsv(img, pre_darken_hsv);
    img = lin2log(img, 1, -20.0f, 2.47393118833f);

    float hdr_mg_pre_darken =
        lin2log(float4(0.18f, 0.18f, 0.18f, 1.0f), 1, -20.0f, 2.47393118833f).x;
    float hdr_mg_darkened =
        lin2log(float4(0.18f / hdr_sdr_ratio, 0.18f, 0.18f, 1.0f), 1, -20.0f, 2.47393118833f).x;
    // slope set to 1.000001 instead of 1.0 to prevent the curve from breaking when hdr_peak_in ==
    // hdr_midgray_factor_in
    img.x = sigmoid(img.x, 1, 3, 1.000001f, hdr_mg_pre_darken, hdr_mg_darkened, 1, 0);
    img.y = sigmoid(img.y, 1, 3, 1.000001f, hdr_mg_pre_darken, hdr_mg_darkened, 1, 0);
    img.z = sigmoid(img.z, 1, 3, 1.000001f, hdr_mg_pre_darken, hdr_mg_darkened, 1, 0);

    img = log2lin(img, 1, -20.0f, 2.47393118833f);

    float4 post_darken_hsv;
    rgb_to_hsv(img, post_darken_hsv);
    post_darken_hsv.x = lerp_chromaticity_angle(
        pre_darken_hsv.x, post_darken_hsv.x, hdr_purity_in);
    post_darken_hsv.y = pre_darken_hsv.y + hdr_purity_in * (post_darken_hsv.y - pre_darken_hsv.y);
    hsv_to_rgb(post_darken_hsv, img);
  }

  // lerp pre- and post-curve chromaticity angle
  float4 post_curve_hsv;
  rgb_to_hsv(img, post_curve_hsv);
  post_curve_hsv[0] = lerp_chromaticity_angle(
      pre_curve_hsv[0], post_curve_hsv[0], hue_flight_strength_in);
  hsv_to_rgb(post_curve_hsv, img);

  // check whether outset matrix is precomputed, if not, calculate it per-pixel
  float4x4 outsetmat_perpixel;
  if (isnan(outsetmat_precomputed[0][0])) {
    float4x4 outset_mat_inv_inset;
    float4x4 outset_mat;

    Chromaticities outset_inv_inset_chromaticities = InsetPrimaries(
        COLOR_SPACE_PRI[int(p_working_primaries)],
        attenuation_rates_in.x,
        attenuation_rates_in.y,
        attenuation_rates_in.z, /* Uses attenuation settings */
        rgb_rotation_in.x,
        rgb_rotation_in.y,
        rgb_rotation_in.z, /* Uses attenuation settings */
        tinting_hue_in + 3.14159265358979323846,
        tinting_scale_in);
    outset_mat_inv_inset = invert(
        RGBtoRGB(outset_inv_inset_chromaticities, COLOR_SPACE_PRI[int(p_working_primaries)]));

    Chromaticities outset_chromaticities = InsetPrimaries(
        COLOR_SPACE_PRI[int(p_working_primaries)],
        restore_purity_in.x,
        restore_purity_in.y,
        restore_purity_in.z,
        reverse_rgb_rotation_in.x,
        reverse_rgb_rotation_in.y,
        reverse_rgb_rotation_in.z,
        tinting_hue_in + 3.14159265358979323846,
        tinting_scale_in);
    outset_mat = invert(
        RGBtoRGB(outset_chromaticities, COLOR_SPACE_PRI[int(p_working_primaries)]));
    outsetmat_perpixel = bool(p_use_inverse_inset) ? outset_mat_inv_inset : outset_mat;
  }

  // apply outset matrix
  if (isnan(outsetmat_precomputed[0][0])) {
    img = outsetmat_perpixel * img;
  }
  else {
    img = outsetmat_precomputed * img;
  }

  // convert from working primaries to target display primaries
  img = working_to_display * img;

  // apply low-side guard rail if the UI checkbox is true, otherwise hard clamp to 0
  if (bool(compensate_negatives_in)) {
    img = compensate_low_side(img, true, display_to_rec2020);
  }
  else {
    img = max(float4(0.0), img);
  }
  img = min(float4(1.0), img);

  float sdr_peak_assumption_for_edr_renormalization = 100.0f;
  if (bool(use_hdr_in)) {
    img *= (hdr_peak_in / sdr_peak_assumption_for_edr_renormalization);
  }

  // convert linearized formed image back to OCIO's scene_linear role space
  img = display_to_scene_linear * img;

  result = img;
}
