#pragma once

#include "BLI_math_base.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"

namespace blender::nodes::node_composite_agx_view_transform_cc {

/* ##########################################################################
    Custom Structs
    ---------------------------
*/

// A struct holding xy for R, G, B, and White point.
struct Chromaticities {
  float2 red;
  float2 green;
  float2 blue;
  float2 white;
};

/* ##########################################################################
    Color Spaces Coordinates
    ---------------------------
*/

static const Chromaticities P3D65_PRI = {
    /* r: */ {0.68f, 0.32f},
    /* g: */ {0.265f, 0.69f},
    /* b: */ {0.15f, 0.06f},
    /* w: */ {0.3127f, 0.3290f}};

static const Chromaticities REC709_PRI = {
    /* r: */ {0.64f, 0.33f},
    /* g: */ {0.3f, 0.6f},
    /* b: */ {0.15f, 0.06f},
    /* w: */ {0.3127f, 0.3290f}};

static const Chromaticities REC2020_PRI = {
    /* r: */ {0.708f, 0.292f},
    /* g: */ {0.17f, 0.797f},
    /* b: */ {0.131f, 0.046f},
    /* w: */ {0.3127f, 0.3290f}};

static const Chromaticities AWG3_PRI = {
    /* r: */ {0.684f, 0.313f},
    /* g: */ {0.221f, 0.848f},
    /* b: */ {0.0861f, -0.102f},
    /* w: */ {0.3127f, 0.3290f}};
// Create a const array of Chromaticities for enum's index-based selection
static const Chromaticities COLOR_SPACE_PRI[] = {
    P3D65_PRI, REC709_PRI, REC2020_PRI, AWG3_PRI};

/* ##########################################################################
    Functions
    ---------------------------------
*/
static inline float3 maxf3(float b, float3 a)
{
  // For each component of float3 a, return max of component and float b
  return float3(
      blender::math::max(a.x, b), blender::math::max(a.y, b), blender::math::max(a.z, b));
}

static inline float3 minf3(float b, float3 a)
{
  // For each component of float3 a, return min of component and float b
  return float3(
      blender::math::min(a.x, b), blender::math::min(a.y, b), blender::math::min(a.z, b));
}

static inline float spowf(float a, float b)
{
  // Compute "safe" power of float a, reflected over the origin

  a = blender::math::sign(a) * pow(fabsf(a), b);
  return a;
}

static inline float3 spowf3(float3 a, float b)
{
  // Compute "safe" power of float3 a, reflected over the origin
  return float3(blender::math::sign(a.x) * pow(fabsf(a.x), b),
                blender::math::sign(a.y) * pow(fabsf(a.y), b),
                blender::math::sign(a.z) * pow(fabsf(a.z), b));
}

static inline float log10f(float x)
{
  return log(x) * (1.0f / log(10.0f));
}

static inline float3 log2lin(float3 rgb,
                             int tf,
                             float generic_log2_min_expo = -10,
                             float generic_log2_max_expo = 6.5)
{
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
  return rgb;
}

static inline float3 lin2log(float3 rgb,
                             int tf,
                             float generic_log2_min_expo = -10,
                             float generic_log2_max_expo = 6.5)
{
  float log_floor =
      log2lin(float3(0.0f, 0.0f, 0.0f), tf, generic_log2_min_expo, generic_log2_max_expo).x;
  rgb = maxf3(log_floor, rgb);
  if (tf == 0) {  // Arri LogC3 EI 800
    rgb.x = rgb.x > 0.010591f ? 0.24719f * log10f(5.555556f * rgb.x + 0.052272f) + 0.385537f :
                                5.367655f * rgb.x + 0.092809f;
    rgb.y = rgb.y > 0.010591f ? 0.24719f * log10f(5.555556f * rgb.y + 0.052272f) + 0.385537f :
                                5.367655f * rgb.y + 0.092809f;
    rgb.z = rgb.z > 0.010591f ? 0.24719f * log10f(5.555556f * rgb.z + 0.052272f) + 0.385537f :
                                5.367655f * rgb.z + 0.092809f;
  }
  else if (tf == 1) {  // User controlled PureLog2
    rgb = float3(log2f(rgb.x / 0.18f), log2f(rgb.y / 0.18f), log2f(rgb.z / 0.18f));
    rgb = minf3(generic_log2_max_expo, rgb);
    rgb = maxf3(generic_log2_min_expo, rgb);
    rgb = (rgb + fabsf(generic_log2_min_expo)) /
          (fabsf(generic_log2_min_expo) + fabsf(generic_log2_max_expo));
  }
  return rgb;
}

static inline float sigmoid(float in,
                            float sp,
                            float tp,
                            float Pslope,
                            float px,
                            float py,
                            float s0 = 1.0f,
                            float t0 = 0.0f)
{
  // calculate Shoulder;
  float ss = spowf(
      ((spowf((Pslope * ((s0 - px) / (1 - py))), sp) - 1) * (spowf(Pslope * (s0 - px), -sp))),
      -1 / sp);
  float ms = Pslope * (in - px) / ss;
  float fs = ms / spowf(1 + (spowf(ms, sp)), 1 / sp);

  // calculate Toe
  float ts = spowf(
      ((spowf((Pslope * ((px - t0) / (py))), tp) - 1) * (spowf(Pslope * (px - t0), -tp))),
      -1 / tp);
  float mr = (Pslope * (in - px)) / -ts;
  float ft = mr / spowf(1 + (spowf(mr, tp)), 1 / tp);

  in = in >= px ? ss * fs + py : (-ts * ft) + py;

  return in;
}

static inline float3x3 RGBtoXYZ(Chromaticities N)
{
  float3x3 M = float3x3(
      float3(N.red.x / N.red.y, 1.0, (1 - N.red.x - N.red.y) / N.red.y),
      float3(N.green.x / N.green.y, 1.0, (1 - N.green.x - N.green.y) / N.green.y),
      float3(N.blue.x / N.blue.y, 1.0, (1 - N.blue.x - N.blue.y) / N.blue.y));
  float3 wh = float3(N.white.x / N.white.y, 1.0, (1 - N.white.x - N.white.y) / N.white.y);
  wh = blender::math::invert(M) * wh;
  M = float3x3(M[0] * wh.x, M[1] * wh.y, M[2] * wh.z);
  return M;
}

static inline float3x3 XYZtoRGB(Chromaticities N)
{
  float3x3 M = blender::math::invert(RGBtoXYZ(N));
  return M;
}

static inline float3x3 RGBtoRGB(Chromaticities N, Chromaticities M)
{
  float3x3 In2XYZ = RGBtoXYZ(N);
  float3x3 XYZ2Out = XYZtoRGB(M);

  float3x3 rgbtorgb = XYZ2Out * In2XYZ;

  return rgbtorgb;
}

static inline float lerp_chromaticity_angle(float h1, float h2, float t)
{
  float delta = h2 - h1;
  if (delta > 0.5f)
    delta -= 1.0f;
  else if (delta < -0.5f)
    delta += 1.0f;
  float lerped = h1 + t * delta;
  return lerped - floorf(lerped);
}

static inline float3 compensate_low_side(float3 rgb,
                                         bool use_heuristic_lerp,
                                         float3x3 input_pri_to_rec2020_mat)
{
  /* Hardcoded Rec.2020 luminance coefficients (2015 10 degree CMFs).
   * Python script to produce the numbers:
   * https://github.com/EaryChow/AgX_LUT_Gen/blob/main/BT2020_2015.py  */
  const float3 luminance_coeffs = float3(
      0.2589235355689848f, 0.6104985346066525f, 0.13057792982436284f);

  float3 rgb_rec2020 = input_pri_to_rec2020_mat * rgb;

  // Calculate original luminance Y
  float Y = rgb_rec2020.x * luminance_coeffs.x + rgb_rec2020.y * luminance_coeffs.y +
            rgb_rec2020.z * luminance_coeffs.z;

  // Calculate inverse RGB in working space
  float max_rgb = fmaxf(rgb.x, fmaxf(rgb.y, rgb.z));
  float3 inverse_rgb = float3(max_rgb - rgb.x, max_rgb - rgb.y, max_rgb - rgb.z);

  // Calculate max of the inverse
  float max_inv_rgb = fmaxf(inverse_rgb.x, fmaxf(inverse_rgb.y, inverse_rgb.z));

  // Convert inverse RGB to Rec.2020 for Y calculation
  float3 inverse_rec2020 = input_pri_to_rec2020_mat * inverse_rgb;
  float Y_inverse = inverse_rec2020.x * luminance_coeffs.x +
                    inverse_rec2020.y * luminance_coeffs.y +
                    inverse_rec2020.z * luminance_coeffs.z;

  // Calculate compensation values
  float y_compensate = (max_inv_rgb - Y_inverse + Y);
  if (use_heuristic_lerp) {
    float Y_clipped = math::clamp(pow(Y, 0.08f), 0.0f, 1.0f);
    y_compensate = y_compensate + Y_clipped * (Y - y_compensate);
  }

  // Offset to avoid negatives
  float min_rgb = fminf(rgb.x, fminf(rgb.y, rgb.z));
  float offset = fmaxf(-min_rgb, 0.0f);
  float3 rgb_offset = float3(rgb.x + offset, rgb.y + offset, rgb.z + offset);

  // Calculate max of the offseted RGB
  float max_offset = fmaxf(rgb_offset.x, fmaxf(rgb_offset.y, rgb_offset.z));

  // Calculate new luminance after offset
  float3 offset_rec2020 = input_pri_to_rec2020_mat * rgb_offset;
  float Y_new = offset_rec2020.x * luminance_coeffs.x + offset_rec2020.y * luminance_coeffs.y +
                offset_rec2020.z * luminance_coeffs.z;

  // Calculate the inverted RGB offset
  float3 inverse_offset = float3(
      max_offset - rgb_offset.x, max_offset - rgb_offset.y, max_offset - rgb_offset.z);

  // Calculate max of the inverse
  float max_inv_offset = fmaxf(inverse_offset.x, fmaxf(inverse_offset.y, inverse_offset.z));

  float3 inverse_offset_rec2020 = input_pri_to_rec2020_mat * inverse_offset;
  float Y_inverse_offset = inverse_offset_rec2020.x * luminance_coeffs.x +
                           inverse_offset_rec2020.y * luminance_coeffs.y +
                           inverse_offset_rec2020.z * luminance_coeffs.z;

  float Y_new_compensate = (max_inv_offset - Y_inverse_offset + Y_new);
  if (use_heuristic_lerp) {
    float Y_new_clipped = math::clamp(pow(Y_new, 0.08f), 0.0f, 1.0f);
    Y_new_compensate = Y_new_compensate + Y_new_clipped * (Y_new - Y_new_compensate);
  }

  // Adjust luminance ratio
  float ratio = (Y_new_compensate > y_compensate) ? (y_compensate / Y_new_compensate) : 1.0f;
  return float3(rgb_offset.x * ratio, rgb_offset.y * ratio, rgb_offset.z * ratio);
}

static inline Chromaticities CenterPrimaries(Chromaticities N)
{
  N.red.x = N.red.x - N.white.x;
  N.red.y = N.red.y - N.white.y;
  N.green.x = N.green.x - N.white.x;
  N.green.y = N.green.y - N.white.y;
  N.blue.x = N.blue.x - N.white.x;
  N.blue.y = N.blue.y - N.white.y;

  return N;
}

static inline Chromaticities DeCenterPrimaries(Chromaticities N)
{
  N.red.x = N.red.x + N.white.x;
  N.red.y = N.red.y + N.white.y;
  N.green.x = N.green.x + N.white.x;
  N.green.y = N.green.y + N.white.y;
  N.blue.x = N.blue.x + N.white.x;
  N.blue.y = N.blue.y + N.white.y;

  return N;
}

static inline float2 cartesian_to_polar2(float2 a)
{
  float2 b = a;
  b.y = atan2(a.y, a.x);

  return float2{sqrt(a.x * a.x + a.y * a.y), b.y};
}

static inline float2 polar_to_cartesian2(float2 a)
{

  return float2{a.x * cos(a.y), a.x * sin(a.y)};
}

static inline Chromaticities RotatePrimary(Chromaticities N, float rrot, float grot, float brot)
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

static inline Chromaticities ScalePrim(Chromaticities N, float rs, float gs, float bs)
{
  N = CenterPrimaries(N);

  N.red = float2{N.red.x * rs, N.red.y * rs};
  N.green = float2{N.green.x * gs, N.green.y * gs};
  N.blue = float2{N.blue.x * bs, N.blue.y * bs};

  N = DeCenterPrimaries(N);

  return N;
}

static inline float2 Line_equation(float2 a, float2 b)
{
  float dx = b.x - a.x;
  if (fabsf(dx) < 1e-6f) {
    /* vertical line → slope = +∞, intercept = x */
    const float kInf = 1e30f * 1e30f;
    return float2{kInf, a.x};
  }
  float m = (b.y - a.y) / dx;
  float c = a.y - m * a.x;
  return float2{m, c};
}

static inline Chromaticities Polygon(Chromaticities N)
{
  Chromaticities M = N;

  N.red = Line_equation(M.red, M.green);
  N.green = Line_equation(M.red, M.blue);
  N.blue = Line_equation(M.blue, M.green);

  return N;
}

static inline Chromaticities PrimariesLines(Chromaticities N)
{
  Chromaticities M = N;

  N.red = Line_equation(M.red, M.white);
  N.green = Line_equation(M.green, M.white);
  N.blue = Line_equation(M.blue, M.white);

  return N;
}

static inline float2 intersection(float2 l1, float2 l2)
{
  float m1 = l1.x, c1 = l1.y;
  float m2 = l2.x, c2 = l2.y;

  bool inf1 = isfinite(m1) == false;
  bool inf2 = isfinite(m2) == false;

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
  return float2{x, y};
}

static inline bool on_segment(float2 P, float2 C, float2 D, float2 A, float2 B)
{
  float t = (abs(B.x - A.x) > abs(B.y - A.y)) ? (P.x - A.x) / (B.x - A.x) :
                                                (P.y - A.y) / (B.y - A.y);
  float u = (abs(D.x - C.x) > abs(D.y - C.y)) ? (P.x - C.x) / (D.x - C.x) :
                                                (P.y - C.y) / (D.y - C.y);
  return (t >= 0.0f && t <= 1.0f && u >= 0.0f && u <= 1.0f);
}

static inline Chromaticities InsetPrimaries(Chromaticities N,
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
  float2 lr = Line_equation(float2{N.red.x, N.red.y}, wp);
  float2 lg = Line_equation(float2{N.green.x, N.green.y}, wp);
  float2 lb = Line_equation(float2{N.blue.x, N.blue.y}, wp);

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
  float2 original_white = Polygon(original_N).white;
  const float arbitrary_scale = 4.0f;

  // Scale & rotate the achromatic point
  float2 scaled_achromatic = float2{original_white.x, original_white.y * arbitrary_scale};
  float dx = scaled_achromatic.x - original_white.x;
  float dy = scaled_achromatic.y - original_white.y;
  float2 rotated_achromatic = float2{
      original_white.x + dx * cos(achromatic_rotate) - dy * sin(achromatic_rotate),
      original_white.y + dx * sin(achromatic_rotate) + dy * cos(achromatic_rotate)};

  // Build the infinite achromatic ray and triangle edges
  float2 la = Line_equation(rotated_achromatic, original_white);
  float2 e1 = Line_equation(original_N.red, original_N.green);
  float2 e2 = Line_equation(original_N.green, original_N.blue);
  float2 e3 = Line_equation(original_N.blue, original_N.red);

  // Find their intersections
  float2 i1 = intersection(la, e1);
  float2 i2 = intersection(la, e2);
  float2 i3 = intersection(la, e3);

  /* Pick the first intersect that lies on both the achromatic ray segment (t∈[0,1])
   * and the edge segment (u∈[0,1]), else fallback to white */
  float2 hull_achromatic = original_white;
  float2 A = rotated_achromatic, B = original_white;

  if (on_segment(i1, original_N.red, original_N.green, A, B))
    hull_achromatic = i1;
  else if (on_segment(i2, original_N.green, original_N.blue, A, B))
    hull_achromatic = i2;
  else if (on_segment(i3, original_N.blue, original_N.red, A, B))
    hull_achromatic = i3;

  // Move whitepoint towards hull_achromatic by achromatic_outset
  float2 interp = float2{(original_white.x - hull_achromatic.x) * (1.0f - achromatic_outset),
                         (original_white.y - hull_achromatic.y) * (1.0f - achromatic_outset)};
  N.white.x = hull_achromatic.x + interp.x;
  N.white.y = hull_achromatic.y + interp.y;

  return N;
}

}  // namespace blender::nodes::node_composite_agx_view_transform_cc
