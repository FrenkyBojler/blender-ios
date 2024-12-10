/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"
#include "BKE_grease_pencil.hh"

#include "DNA_grease_pencil_types.h"

#include "attribute_access_intern.hh"

namespace blender::bke::greasepencil {

static void tag_component_opacities_changed(void *owner)
{
  CurvesGeometry &curves = *static_cast<CurvesGeometry *>(owner);
  // TODO
}

static void tag_component_vertex_colors_changed(void *owner)
{
  CurvesGeometry &curves = *static_cast<CurvesGeometry *>(owner);
  // TODO
}

static void tag_component_fill_colors_changed(void *owner)
{
  CurvesGeometry &curves = *static_cast<CurvesGeometry *>(owner);
  // TODO
}

static void tag_component_fill_opacities_changed(void *owner)
{
  CurvesGeometry &curves = *static_cast<CurvesGeometry *>(owner);
  // TODO
}

static void tag_component_aspect_ratios_changed(void *owner)
{
  CurvesGeometry &curves = *static_cast<CurvesGeometry *>(owner);
  // TODO
}

static void tag_component_u_scales_changed(void *owner)
{
  CurvesGeometry &curves = *static_cast<CurvesGeometry *>(owner);
  // TODO
}

static GeometryAttributeProviders create_attribute_providers_for_grease_pencil_drawing()
{
  GeometryAttributeProviders providers = bke::curves::create_attribute_providers_for_curve();

  static CustomDataAccessInfo curve_access = {
      [](void *owner) -> CustomData * {
        CurvesGeometry &curves = *static_cast<CurvesGeometry *>(owner);
        return &curves.curve_data;
      },
      [](const void *owner) -> const CustomData * {
        const CurvesGeometry &curves = *static_cast<const CurvesGeometry *>(owner);
        return &curves.curve_data;
      },
      [](const void *owner) -> int {
        const CurvesGeometry &curves = *static_cast<const CurvesGeometry *>(owner);
        return curves.curves_num();
      }};
  static CustomDataAccessInfo point_access = {
      [](void *owner) -> CustomData * {
        CurvesGeometry &curves = *static_cast<CurvesGeometry *>(owner);
        return &curves.point_data;
      },
      [](const void *owner) -> const CustomData * {
        const CurvesGeometry &curves = *static_cast<const CurvesGeometry *>(owner);
        return &curves.point_data;
      },
      [](const void *owner) -> int {
        const CurvesGeometry &curves = *static_cast<const CurvesGeometry *>(owner);
        return curves.points_num();
      }};

  static BuiltinCustomDataLayerProvider opacities("opacity",
                                                  bke::AttrDomain::Point,
                                                  CD_PROP_FLOAT,
                                                  BuiltinAttributeProvider::Deletable,
                                                  point_access,
                                                  tag_component_opacities_changed);
  providers.add_builtin_attribute_provider(&opacities);

  static ColorGeometry4f default_vertex_color(0.0f, 0.0f, 0.0f, 0.0f);
  static BuiltinCustomDataLayerProvider vertex_colors("vertex_color",
                                                      bke::AttrDomain::Point,
                                                      CD_PROP_COLOR,
                                                      BuiltinAttributeProvider::Deletable,
                                                      point_access,
                                                      tag_component_vertex_colors_changed,
                                                      {},
                                                      &default_vertex_color);
  providers.add_builtin_attribute_provider(&vertex_colors);

  static ColorGeometry4f default_fill_color(0.0f, 0.0f, 0.0f, 0.0f);
  static BuiltinCustomDataLayerProvider fill_colors("fill_color",
                                                    bke::AttrDomain::Curve,
                                                    CD_PROP_COLOR,
                                                    BuiltinAttributeProvider::Deletable,
                                                    curve_access,
                                                    tag_component_fill_colors_changed,
                                                    {},
                                                    &default_fill_color);
  providers.add_builtin_attribute_provider(&fill_colors);

  static float default_fill_opacity = 1.0f;
  static BuiltinCustomDataLayerProvider fill_opacities("fill_opacity",
                                                       bke::AttrDomain::Curve,
                                                       CD_PROP_FLOAT,
                                                       BuiltinAttributeProvider::Deletable,
                                                       curve_access,
                                                       tag_component_fill_opacities_changed,
                                                       {},
                                                       &default_fill_opacity);
  providers.add_builtin_attribute_provider(&fill_opacities);

  static float default_aspect_ratio = 1.0f;
  static BuiltinCustomDataLayerProvider aspect_ratios("aspect_ratio",
                                                      bke::AttrDomain::Curve,
                                                      CD_PROP_FLOAT,
                                                      BuiltinAttributeProvider::Deletable,
                                                      curve_access,
                                                      tag_component_aspect_ratios_changed,
                                                      {},
                                                      &default_aspect_ratio);
  providers.add_builtin_attribute_provider(&aspect_ratios);

  static float default_u_scale = 1.0f;
  static BuiltinCustomDataLayerProvider u_scales("u_scale",
                                                 bke::AttrDomain::Curve,
                                                 CD_PROP_FLOAT,
                                                 BuiltinAttributeProvider::Deletable,
                                                 curve_access,
                                                 tag_component_u_scales_changed,
                                                 {},
                                                 &default_u_scale);
  providers.add_builtin_attribute_provider(&u_scales);

  return providers;
}

static GeometryAttributeProviders create_attribute_providers_for_grease_pencil()
{
  static CustomDataAccessInfo layers_access = {
      [](void *owner) -> CustomData * {
        GreasePencil &grease_pencil = *static_cast<GreasePencil *>(owner);
        return &grease_pencil.layers_data;
      },
      [](const void *owner) -> const CustomData * {
        const GreasePencil &grease_pencil = *static_cast<const GreasePencil *>(owner);
        return &grease_pencil.layers_data;
      },
      [](const void *owner) -> int {
        const GreasePencil &grease_pencil = *static_cast<const GreasePencil *>(owner);
        return grease_pencil.layers().size();
      }};

  static CustomDataAttributeProvider layer_custom_data(AttrDomain::Layer, layers_access);

  return GeometryAttributeProviders({}, {&layer_custom_data});
}

static GVArray adapt_grease_pencil_attribute_domain(const GreasePencil & /*grease_pencil*/,
                                                    const GVArray &varray,
                                                    const AttrDomain from,
                                                    const AttrDomain to)
{
  if (from == to) {
    return varray;
  }
  return {};
}

static AttributeAccessorFunctions get_grease_pencil_accessor_functions()
{
  static const GeometryAttributeProviders providers =
      create_attribute_providers_for_grease_pencil();
  AttributeAccessorFunctions fn =
      attribute_accessor_functions::accessor_functions_for_providers<providers>();
  fn.domain_size = [](const void *owner, const AttrDomain domain) {
    if (owner == nullptr) {
      return 0;
    }
    const GreasePencil &grease_pencil = *static_cast<const GreasePencil *>(owner);
    switch (domain) {
      case AttrDomain::Layer:
        return int(grease_pencil.layers().size());
      default:
        return 0;
    }
  };
  fn.domain_supported = [](const void * /*owner*/, const AttrDomain domain) {
    return domain == AttrDomain::Layer;
  };
  fn.adapt_domain = [](const void *owner,
                       const GVArray &varray,
                       const AttrDomain from_domain,
                       const AttrDomain to_domain) -> GVArray {
    if (owner == nullptr) {
      return {};
    }
    const GreasePencil &grease_pencil = *static_cast<const GreasePencil *>(owner);
    return adapt_grease_pencil_attribute_domain(grease_pencil, varray, from_domain, to_domain);
  };
  return fn;
}

const AttributeAccessorFunctions &get_attribute_accessor_functions()
{
  static const AttributeAccessorFunctions fn = get_grease_pencil_accessor_functions();
  return fn;
}

static AttributeAccessorFunctions get_grease_pencil_drawing_accessor_functions()
{
  static const GeometryAttributeProviders providers =
      create_attribute_providers_for_grease_pencil_drawing();
  AttributeAccessorFunctions fn =
      attribute_accessor_functions::accessor_functions_for_providers<providers>();
  fn.domain_size = [](const void *owner, const AttrDomain domain) {
    if (owner == nullptr) {
      return 0;
    }
    const CurvesGeometry &curves = *static_cast<const CurvesGeometry *>(owner);
    switch (domain) {
      case AttrDomain::Point:
        return curves.points_num();
      case AttrDomain::Curve:
        return curves.curves_num();
      default:
        return 0;
    }
  };
  fn.domain_supported = [](const void * /*owner*/, const AttrDomain domain) {
    return ELEM(domain, AttrDomain::Point, AttrDomain::Curve);
  };
  fn.adapt_domain = [](const void *owner,
                       const GVArray &varray,
                       const AttrDomain from_domain,
                       const AttrDomain to_domain) -> GVArray {
    if (owner == nullptr) {
      return {};
    }
    const CurvesGeometry &curves = *static_cast<const CurvesGeometry *>(owner);
    return curves.adapt_domain(varray, from_domain, to_domain);
  };
  return fn;
}

namespace drawing {

const AttributeAccessorFunctions &get_attribute_accessor_functions()
{
  static const AttributeAccessorFunctions fn = get_grease_pencil_drawing_accessor_functions();
  return fn;
}

}  // namespace drawing

}  // namespace blender::bke::greasepencil
