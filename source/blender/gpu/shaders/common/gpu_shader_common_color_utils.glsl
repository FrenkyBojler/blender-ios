/* SPDX-FileCopyrightText: 2019-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_compat.hh"

[[node]]
void rgb_to_hsv(float4 rgb, float4 &outcol)
{
  float cmax, cmin, h, s, v, cdelta;
  float3 c;

  cmax = max(rgb[0], max(rgb[1], rgb[2]));
  cmin = min(rgb[0], min(rgb[1], rgb[2]));
  cdelta = cmax - cmin;

  v = cmax;
  if (cmax != 0.0f) {
    s = cdelta / cmax;
  }
  else {
    s = 0.0f;
    h = 0.0f;
  }

  if (s == 0.0f) {
    h = 0.0f;
  }
  else {
    c = (float3(cmax) - rgb.xyz) / cdelta;

    if (rgb.x == cmax) {
      h = c[2] - c[1];
    }
    else if (rgb.y == cmax) {
      h = 2.0f + c[0] - c[2];
    }
    else {
      h = 4.0f + c[1] - c[0];
    }

    h /= 6.0f;

    if (h < 0.0f) {
      h += 1.0f;
    }
  }

  outcol = float4(h, s, v, rgb.w);
}

[[node]]
void hsv_to_rgb(float4 hsv, float4 &outcol)
{
  float i, f, p, q, t, h, s, v;
  float3 rgb;

  h = hsv[0];
  s = hsv[1];
  v = hsv[2];

  if (s == 0.0f) {
    rgb = float3(v, v, v);
  }
  else {
    if (h == 1.0f) {
      h = 0.0f;
    }

    h *= 6.0f;
    i = floor(h);
    f = h - i;
    rgb = float3(f, f, f);
    p = v * (1.0f - s);
    q = v * (1.0f - (s * f));
    t = v * (1.0f - (s * (1.0f - f)));

    if (i == 0.0f) {
      rgb = float3(v, t, p);
    }
    else if (i == 1.0f) {
      rgb = float3(q, v, p);
    }
    else if (i == 2.0f) {
      rgb = float3(p, v, t);
    }
    else if (i == 3.0f) {
      rgb = float3(p, q, v);
    }
    else if (i == 4.0f) {
      rgb = float3(t, p, v);
    }
    else {
      rgb = float3(v, p, q);
    }
  }

  outcol = float4(rgb, hsv.w);
}

[[node]]
void rgb_to_hsl(float4 rgb, float4 &outcol)
{
  float cmax, cmin, h, s, l;

  cmax = max(rgb[0], max(rgb[1], rgb[2]));
  cmin = min(rgb[0], min(rgb[1], rgb[2]));
  l = min(1.0f, (cmax + cmin) / 2.0f);

  if (cmax == cmin) {
    h = s = 0.0f; /* achromatic */
  }
  else {
    float cdelta = cmax - cmin;
    s = l > 0.5f ? cdelta / (2.0f - cmax - cmin) : cdelta / (cmax + cmin);
    if (cmax == rgb[0]) {
      h = (rgb[1] - rgb[2]) / cdelta + (rgb[1] < rgb[2] ? 6.0f : 0.0f);
    }
    else if (cmax == rgb[1]) {
      h = (rgb[2] - rgb[0]) / cdelta + 2.0f;
    }
    else {
      h = (rgb[0] - rgb[1]) / cdelta + 4.0f;
    }
  }
  h /= 6.0f;

  outcol = float4(h, s, l, rgb.w);
}

[[node]]
void hsl_to_rgb(float4 hsl, float4 &outcol)
{
  float nr, ng, nb, chroma, h, s, l;

  h = hsl[0];
  s = hsl[1];
  l = hsl[2];

  nr = abs(h * 6.0f - 3.0f) - 1.0f;
  ng = 2.0f - abs(h * 6.0f - 2.0f);
  nb = 2.0f - abs(h * 6.0f - 4.0f);

  nr = clamp(nr, 0.0f, 1.0f);
  nb = clamp(nb, 0.0f, 1.0f);
  ng = clamp(ng, 0.0f, 1.0f);

  chroma = (1.0f - abs(2.0f * l - 1.0f)) * s;

  outcol = float4(
      (nr - 0.5f) * chroma + l, (ng - 0.5f) * chroma + l, (nb - 0.5f) * chroma + l, hsl.w);
}

[[node]]
void rgb_to_lab(float4 rgb, float4 &outcol)
{
  float l, m, s;

  l = 0.4122214708f * rgb[0] + 0.5363325363f * rgb[1] + 0.0514459929f * rgb[2];
	m = 0.2119034982f * rgb[0] + 0.6806995451f * rgb[1] + 0.1073969566f * rgb[2];
	s = 0.0883024619f * rgb[0] + 0.2817188376f * rgb[1] + 0.6299787005f * rgb[2];

  l = pow(l, 1.0f/3.0f);
  m = pow(m, 1.0f/3.0f);
  s = pow(s, 1.0f/3.0f);

  outcol = float4(
    0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
    1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
    0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s,
    rgb[3]
  );
}

[[node]]
void lab_to_rgb(float4 lab, float4 &outcol)
{
  float l, m, s;

  l = lab[0] + 0.3963377774f * lab[1] + 0.2158037573f * lab[2];
  m = lab[0] - 0.1055613458f * lab[1] - 0.0638541728f * lab[2];
  s = lab[0] - 0.0894841775f * lab[1] - 1.2914855480f * lab[2];

  l = l * l * l;
  m = m * m * m;
  s = s * s * s;

  outcol = float4(
    +4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s,
    -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
    -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s,
    lab[3]
  );
}

[[node]]
void rgb_to_lch(float4 rgb, float4 &outcol)
{
  float c, h;
  float4 lab;
  rgb_to_lab(rgb, lab);

  c = sqrt(lab[1] * lab[1] + lab[2] * lab[2]) * 4.0f;
  h = atan(lab[2], lab[1]) / radians(360.0f);

  outcol = float4(lab[0], c, h, lab[3]);
}

[[node]]
void lch_to_rgb(float4 lch, float4 &outcol)
{
  float a, b;

  a = lch[1] * cos(lch[2] * radians(360.0f)) / 4.0f;
  b = lch[1] * sin(lch[2] * radians(360.0f)) / 4.0f;

  lab_to_rgb(float4(lch[0], a, b, lch[3]), outcol);
}

/* ** YCCA to RGBA ** */

[[node]]
void ycca_to_rgba_itu_601(float4 ycca, float4 &color)
{
  ycca.xyz *= 255.0f;
  ycca.xyz -= float3(16.0f, 128.0f, 128.0f);
  color.rgb = float3x3(1.164f, 1.164f, 1.164f, 0.0f, -0.392f, 2.017f, 1.596f, -0.813f, 0.0f) *
              ycca.xyz;
  color.rgb /= 255.0f;
  color.a = ycca.a;
}

[[node]]
void ycca_to_rgba_itu_709(float4 ycca, float4 &color)
{
  ycca.xyz *= 255.0f;
  ycca.xyz -= float3(16.0f, 128.0f, 128.0f);
  color.rgb = float3x3(1.164f, 1.164f, 1.164f, 0.0f, -0.213f, 2.115f, 1.793f, -0.534f, 0.0f) *
              ycca.xyz;
  color.rgb /= 255.0f;
  color.a = ycca.a;
}

[[node]]
void ycca_to_rgba_jpeg(float4 ycca, float4 &color)
{
  ycca.xyz *= 255.0f;
  color.rgb = float3x3(1.0f, 1.0f, 1.0f, 0.0f, -0.34414f, 1.772f, 1.402f, -0.71414f, 0.0f) *
              ycca.xyz;
  color.rgb += float3(-179.456f, 135.45984f, -226.816f);
  color.rgb /= 255.0f;
  color.a = ycca.a;
}

/* ** RGBA to YCCA ** */

[[node]]
void rgba_to_ycca_itu_601(float4 rgba, float4 &ycca)
{
  rgba.rgb *= 255.0f;
  ycca.xyz = float3x3(0.257f, -0.148f, 0.439f, 0.504f, -0.291f, -0.368f, 0.098f, 0.439f, -0.071f) *
             rgba.rgb;
  ycca.xyz += float3(16.0f, 128.0f, 128.0f);
  ycca.xyz /= 255.0f;
  ycca.a = rgba.a;
}

[[node]]
void rgba_to_ycca_itu_709(float4 rgba, float4 &ycca)
{
  rgba.rgb *= 255.0f;
  ycca.xyz = float3x3(0.183f, -0.101f, 0.439f, 0.614f, -0.338f, -0.399f, 0.062f, 0.439f, -0.040f) *
             rgba.rgb;
  ycca.xyz += float3(16.0f, 128.0f, 128.0f);
  ycca.xyz /= 255.0f;
  ycca.a = rgba.a;
}

[[node]]
void rgba_to_ycca_jpeg(float4 rgba, float4 &ycca)
{
  rgba.rgb *= 255.0f;
  ycca.xyz = float3x3(
                 0.299f, -0.16874f, 0.5f, 0.587f, -0.33126f, -0.41869f, 0.114f, 0.5f, -0.08131f) *
             rgba.rgb;
  ycca.xyz += float3(0.0f, 128.0f, 128.0f);
  ycca.xyz /= 255.0f;
  ycca.a = rgba.a;
}

/* ** YUVA to RGBA ** */

[[node]]
void yuva_to_rgba_itu_709(float4 yuva, float4 &color)
{
  color.rgb = float3x3(1.0f, 1.0f, 1.0f, 0.0f, -0.21482f, 2.12798f, 1.28033f, -0.38059f, 0.0f) *
              yuva.xyz;
  color.a = yuva.a;
}

/* ** RGBA to YUVA ** */

[[node]]
void rgba_to_yuva_itu_709(float4 rgba, float4 &yuva)
{
  yuva.xyz =
      float3x3(
          0.2126f, -0.09991f, 0.615f, 0.7152f, -0.33609f, -0.55861f, 0.0722f, 0.436f, -0.05639f) *
      rgba.rgb;
  yuva.a = rgba.a;
}

/* ** Alpha Handling ** */

[[node]]
void color_alpha_clear(float4 color, float4 &result)
{
  result = float4(color.rgb, 1.0f);
}

[[node]]
void color_alpha_premultiply(float4 color, float4 &result)
{
  result = float4(color.rgb * color.a, color.a);
}

[[node]]
void color_alpha_unpremultiply(float4 color, float4 &result)
{
  if (color.a == 0.0f || color.a == 1.0f) {
    result = color;
  }
  else {
    result = float4(color.rgb / color.a, color.a);
  }
}

float linear_rgb_to_srgb(float color)
{
  if (color < 0.0031308f) {
    return (color < 0.0f) ? 0.0f : color * 12.92f;
  }

  return 1.055f * pow(color, 1.0f / 2.4f) - 0.055f;
}

float3 linear_rgb_to_srgb(float3 color)
{
  return float3(
      linear_rgb_to_srgb(color.r), linear_rgb_to_srgb(color.g), linear_rgb_to_srgb(color.b));
}

float srgb_to_linear_rgb(float color)
{
  if (color < 0.04045f) {
    return (color < 0.0f) ? 0.0f : color * (1.0f / 12.92f);
  }

  return pow((color + 0.055f) * (1.0f / 1.055f), 2.4f);
}

float3 srgb_to_linear_rgb(float3 color)
{
  return float3(
      srgb_to_linear_rgb(color.r), srgb_to_linear_rgb(color.g), srgb_to_linear_rgb(color.b));
}

float get_luminance(float3 color, float3 luminance_coefficients)
{
  return dot(color, luminance_coefficients);
}
