/* SPDX-FileCopyrightText: 2021 NVIDIA Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "usd_reader_domelight.hh"
#include "usd_light_convert.hh"

#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdLux/domeLight_1.h>

namespace usdtokens {
// Attribute names.
static const pxr::TfToken color("color", pxr::TfToken::Immortal);
static const pxr::TfToken intensity("intensity", pxr::TfToken::Immortal);
static const pxr::TfToken texture_file("texture:file", pxr::TfToken::Immortal);
}  // namespace usdtokens

namespace blender::io::usd {

/**
 * If the given attribute has an authored value, return its value in the r_value
 * out parameter.
 *
 * We wish to support older UsdLux APIs in older versions of USD.  For example,
 * in previous versions of the API, shader input attributes did not have the
 * "inputs:" prefix.  One can provide the older input attribute name in the
 * 'fallback_attr_name' argument, and that attribute will be queried if 'attr'
 * doesn't exist or doesn't have an authored value.
 */
template<typename T>
bool get_authored_value(const pxr::UsdAttribute &attr,
                        const double motionSampleTime,
                        const pxr::UsdPrim &prim,
                        const pxr::TfToken fallback_attr_name,
                        T *r_value)
{
  if (attr && attr.HasAuthoredValue()) {
    return attr.Get<T>(r_value, motionSampleTime);
  }

  if (!prim || fallback_attr_name.IsEmpty()) {
    return false;
  }

  pxr::UsdAttribute fallback_attr = prim.GetAttribute(fallback_attr_name);
  if (fallback_attr && fallback_attr.HasAuthoredValue()) {
    return fallback_attr.Get<T>(r_value, motionSampleTime);
  }

  return false;
}

template<typename T> float get_domeligth_intensity(T dome_light, float motionSampleTime)
{
  float intensity = 1.0f;
  get_authored_value(dome_light.GetIntensityAttr(),
                     motionSampleTime,
                     dome_light.GetPrim(),
                     usdtokens::intensity,
                     &intensity);
  return intensity;
}

template<typename T>
bool get_domeligth_tex_path(T dome_light, float motionSampleTime, pxr::SdfAssetPath *tex_path)
{
  bool has_tex = get_authored_value(dome_light.GetTextureFileAttr(),
                                    motionSampleTime,
                                    dome_light.GetPrim(),
                                    usdtokens::texture_file,
                                    tex_path);
  return has_tex;
}

template<typename T>
bool get_domeligth_color(T dome_light, float motionSampleTime, pxr::GfVec3f *color)
{
  bool has_color = get_authored_value(
      dome_light.GetColorAttr(), motionSampleTime, dome_light.GetPrim(), usdtokens::color, color);
  return has_color;
}

void USDDomeLightReader::create_object(Scene *scene, Main *bmain)
{
  USDImportDomeLightAttr attr;

  const double motionSampleTime = 0.0;

  if (prim_.IsA<pxr::UsdLuxDomeLight>()) {
    pxr::UsdLuxDomeLight dome_light = pxr::UsdLuxDomeLight(prim_);
    attr.intensity = get_domeligth_intensity(dome_light, motionSampleTime);
    attr.has_tex = get_domeligth_tex_path(dome_light, motionSampleTime, &attr.tex_path);
    attr.has_color = get_domeligth_color(dome_light, motionSampleTime, &attr.color);
  }
  else if (prim_.IsA<pxr::UsdLuxDomeLight_1>()) {
    pxr::UsdLuxDomeLight_1 dome_light = pxr::UsdLuxDomeLight_1(prim_);
    attr.intensity = get_domeligth_intensity(dome_light, motionSampleTime);
    attr.has_tex = get_domeligth_tex_path(dome_light, motionSampleTime, &attr.tex_path);
    attr.has_color = get_domeligth_color(dome_light, motionSampleTime, &attr.color);
  }

  dome_light_to_world_material(import_params_, scene, bmain, attr, prim_);
}

}  // namespace blender::io::usd
