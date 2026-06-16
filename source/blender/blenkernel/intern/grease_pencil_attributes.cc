/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_listbase.hh"

#include "BKE_attribute_storage.hh"
#include "BKE_curves.hh"
#include "BKE_deform.hh"
#include "BKE_grease_pencil.hh"

#include "DNA_grease_pencil_types.h"

#include "FN_multi_function_builder.hh"

#include "attribute_storage_access.hh"
#include "curves_attributes.hh"

namespace blender::bke::greasepencil {

namespace drawing {

static int get_domain_size(const void *owner, const AttrDomain domain)
{
  const Drawing &drawing = *static_cast<const Drawing *>(owner);
  switch (domain) {
    case AttrDomain::Point:
      return drawing.geometry.wrap().points_num();
    case AttrDomain::Curve:
      return drawing.geometry.wrap().curves_num();
    default:
      return 0;
  }
}

static const auto &changed_tags()
{
  static Map<StringRef, AttrUpdateOnChange> attributes{
      /* TODO. */
  };
  return attributes;
}

static GAttributeReader reader_for_vertex_group_index(const CurvesGeometry &curves,
                                                      const Span<MDeformVert> dverts,
                                                      const int vertex_group_index)
{
  BLI_assert(vertex_group_index >= 0);
  if (dverts.is_empty()) {
    return {VArray<float>::from_single(0.0f, curves.points_num()), AttrDomain::Point};
  }
  return {varray_for_deform_verts(dverts, vertex_group_index), AttrDomain::Point};
}

static GAttributeReader try_get_vertex_group(const void *owner, const StringRef name)
{
  if (owner == nullptr) {
    return {};
  }
  const Drawing &drawing = *static_cast<const Drawing *>(owner);
  const CurvesGeometry &curves = drawing.as_curves();

  const int vertex_group_index = BKE_defgroup_name_index(&curves.vertex_group_names, name);
  if (vertex_group_index < 0) {
    return {};
  }
  const Span<MDeformVert> dverts = curves.deform_verts();
  return reader_for_vertex_group_index(curves, dverts, vertex_group_index);
}

static GAttributeWriter try_get_vertex_group_for_write(void *owner, const StringRef name)
{
  if (owner == nullptr) {
    return {};
  }
  Drawing &drawing = *static_cast<Drawing *>(owner);
  CurvesGeometry &curves = drawing.as_curves_for_write();

  const int vertex_group_index = BKE_defgroup_name_index(&curves.vertex_group_names, name);
  if (vertex_group_index < 0) {
    return {};
  }
  MutableSpan<MDeformVert> dverts = curves.deform_verts_for_write();
  return {varray_for_mutable_deform_verts(dverts, vertex_group_index), AttrDomain::Point};
}

static bool foreach_vertex_group(const void *owner, FunctionRef<void(const AttributeIter &)> fn)
{
  if (owner == nullptr) {
    return true;
  }
  const Drawing &drawing = *static_cast<const Drawing *>(owner);
  const CurvesGeometry &curves = drawing.as_curves();

  const AttributeAccessor accessor = curves.attributes();
  const Span<MDeformVert> dverts = curves.deform_verts();

  for (const auto [group_index, group] : curves.vertex_group_names.enumerate()) {
    const auto get_fn = [&, group_index = group_index]() {
      return reader_for_vertex_group_index(curves, dverts, group_index);
    };
    AttributeIter iter{group.name, AttrDomain::Point, bke::AttrType::Float, get_fn};
    iter.is_builtin = false;
    iter.accessor = &accessor;
    fn(iter);
    if (iter.is_stopped()) {
      return false;
    }
  }
  return true;
}

static const auto &builtin_attributes()
{
  static auto attributes = []() {
    /* Grease Pencil drawings use `CurvesGeometry` as the foundation so use the existing builtin
     * curves attributes. */
    Map<StringRef, AttrBuiltinInfo> map = bke::curves::get_builtin_attributes_map();

    static float default_opacity = 1.0f;
    AttrBuiltinInfo opacity(AttrDomain::Point, AttrType::Float);
    opacity.default_value = &default_opacity;
    map.add_new("opacity", std::move(opacity));

    AttrBuiltinInfo rotation(AttrDomain::Point, AttrType::Float);
    map.add_new("rotation", std::move(rotation));

    static auto miter_angle_clamp = mf::build::SI1_SO<float, float>(
        "Miter Angle Validate",
        [](float value) {
          return std::clamp<float>(
              value, GP_STROKE_MITER_ANGLE_ROUND, GP_STROKE_MITER_ANGLE_BEVEL);
        },
        mf::build::exec_presets::AllSpanOrSingle());
    AttrBuiltinInfo miter_angle(AttrDomain::Point, AttrType::Float);
    miter_angle.validator = AttributeValidator{&miter_angle_clamp};
    map.add_new("miter_angle", std::move(miter_angle));

    AttrBuiltinInfo vertex_color(AttrDomain::Point, AttrType::ColorFloat);
    map.add_new("vertex_color", std::move(vertex_color));

    AttrBuiltinInfo delta_time(AttrDomain::Point, AttrType::Float);
    map.add_new("delta_time", std::move(delta_time));

    static auto caps_type_clamp = mf::build::SI1_SO<int8_t, int8_t>(
        "Cap Type Validate",
        [](int8_t value) {
          return std::clamp<int8_t>(value, GP_STROKE_CAP_TYPE_ROUND, GP_STROKE_CAP_TYPE_FLAT);
        },
        mf::build::exec_presets::AllSpanOrSingle());
    AttrBuiltinInfo start_cap(AttrDomain::Curve, AttrType::Int8);
    start_cap.validator = AttributeValidator{&caps_type_clamp};
    map.add_new("start_cap", std::move(start_cap));

    AttrBuiltinInfo end_cap(AttrDomain::Curve, AttrType::Int8);
    end_cap.validator = AttributeValidator{&caps_type_clamp};
    map.add_new("end_cap", std::move(end_cap));

    AttrBuiltinInfo hide_stroke(AttrDomain::Curve, AttrType::Bool);
    map.add_new("hide_stroke", std::move(hide_stroke));

    AttrBuiltinInfo fill_color(AttrDomain::Curve, AttrType::ColorFloat);
    map.add_new("fill_color", std::move(fill_color));

    static float default_fill_opacity = 1.0f;
    AttrBuiltinInfo fill_opacity(AttrDomain::Curve, AttrType::Float);
    fill_opacity.default_value = &default_fill_opacity;
    map.add_new("fill_opacity", std::move(fill_opacity));

    AttrBuiltinInfo fill_id(AttrDomain::Curve, AttrType::Int32);
    map.add_new("fill_id", std::move(fill_id));

    AttrBuiltinInfo softness(AttrDomain::Curve, AttrType::Float);
    map.add_new("softness", std::move(softness));

    static float default_aspect_ratio = 1.0f;
    AttrBuiltinInfo aspect_ratio(AttrDomain::Curve, AttrType::Float);
    aspect_ratio.default_value = &default_aspect_ratio;
    map.add_new("aspect_ratio", std::move(aspect_ratio));

    AttrBuiltinInfo u_translation(AttrDomain::Curve, AttrType::Float);
    map.add_new("u_translation", std::move(u_translation));

    static float default_u_scale = 1.0f;
    AttrBuiltinInfo u_scale(AttrDomain::Curve, AttrType::Float);
    u_scale.default_value = &default_u_scale;
    map.add_new("u_scale", std::move(u_scale));

    AttrBuiltinInfo init_time(AttrDomain::Curve, AttrType::Float);
    map.add_new("init_time", std::move(init_time));

    return map;
  }();
  return attributes;
}

static const auto &array_storage_required()
{
  static Set<StringRef> attributes{"position", "handle_left", "handle_right", "nurbs_weight"};
  return attributes;
}

static AttributeAccessorFunctions get_grease_pencil_drawing_accessor_functions()
{
  AttributeAccessorFunctions fn{};
  fn.domain_supported = [](const void * /*owner*/, const AttrDomain domain) {
    return ELEM(domain, AttrDomain::Point, AttrDomain::Curve);
  };
  fn.domain_size = get_domain_size;
  fn.builtin_domain_and_type = [](const void * /*owner*/,
                                  const StringRef name) -> std::optional<AttributeDomainAndType> {
    const AttrBuiltinInfo *info = builtin_attributes().lookup_ptr(name);
    if (!info) {
      return std::nullopt;
    }
    return AttributeDomainAndType{info->domain, info->type};
  };
  fn.get_builtin_default = [](const void * /*owner*/, StringRef name) -> GPointer {
    const AttrBuiltinInfo &info = builtin_attributes().lookup(name);
    return info.default_value;
  };
  fn.lookup_meta_data = [](const void *owner, StringRef name) -> std::optional<AttributeMetaData> {
    const Drawing &drawing = *static_cast<const Drawing *>(owner);
    const CurvesGeometry &curves = drawing.as_curves();
    if (BKE_defgroup_name_index(&curves.vertex_group_names, name) != -1) {
      return AttributeMetaData{AttrDomain::Point, AttrType::Float};
    }
    const AttributeStorage &storage = curves.attribute_storage.wrap();
    const Attribute *attr = storage.lookup(name);
    if (!attr) {
      return std::nullopt;
    }
    return AttributeMetaData{attr->domain(), attr->data_type()};
  };
  fn.lookup = [](const void *owner, const StringRef name) -> GAttributeReader {
    const Drawing &drawing = *static_cast<const Drawing *>(owner);
    const CurvesGeometry &curves = drawing.as_curves();
    if (GAttributeReader vertex_group = try_get_vertex_group(owner, name)) {
      return vertex_group;
    }

    const AttributeStorage &storage = curves.attribute_storage.wrap();
    const Attribute *attr = storage.lookup(name);
    if (!attr) {
      return {};
    }
    const int domain_size = get_domain_size(owner, attr->domain());
    return attribute_to_reader(*attr, attr->domain(), domain_size);
  };
  fn.adapt_domain = [](const void *owner,
                       const GVArray &varray,
                       const AttrDomain from_domain,
                       const AttrDomain to_domain) -> GVArray {
    const Drawing &drawing = *static_cast<const Drawing *>(owner);
    const CurvesGeometry &curves = drawing.as_curves();
    return curves.adapt_domain(varray, from_domain, to_domain);
  };
  fn.foreach_attribute = [](const void *owner,
                            const FunctionRef<void(const AttributeIter &)> fn,
                            const AttributeAccessor &accessor) {
    const Drawing &drawing = *static_cast<const Drawing *>(owner);
    const CurvesGeometry &curves = drawing.as_curves();

    const bool should_continue = foreach_vertex_group(owner, fn);
    if (!should_continue) {
      return;
    }

    const AttributeStorage &storage = curves.attribute_storage.wrap();
    for (const Attribute &attr : storage) {
      const auto get_fn = [&]() {
        const int domain_size = get_domain_size(owner, attr.domain());
        return attribute_to_reader(attr, attr.domain(), domain_size);
      };
      AttributeIter iter(attr.name(), attr.domain(), attr.data_type(), get_fn);
      iter.is_builtin = builtin_attributes().contains(attr.name());
      iter.storage_type = attr.storage_type();
      iter.accessor = &accessor;
      fn(iter);
      if (iter.is_stopped()) {
        break;
      }
    }
  };
  fn.lookup_validator = [](const void * /*owner*/, const StringRef name) -> AttributeValidator {
    const AttrBuiltinInfo *info = builtin_attributes().lookup_ptr(name);
    if (!info) {
      return {};
    }
    return info->validator;
  };
  fn.lookup_for_write = [](void *owner, const StringRef name) -> GAttributeWriter {
    Drawing &drawing = *static_cast<Drawing *>(owner);
    CurvesGeometry &curves = drawing.as_curves_for_write();

    if (GAttributeWriter vertex_group = try_get_vertex_group_for_write(owner, name)) {
      return vertex_group;
    }

    AttributeStorage &storage = curves.attribute_storage.wrap();
    Attribute *attr = storage.lookup(name);
    if (!attr) {
      return {};
    }
    const int domain_size = get_domain_size(owner, attr->domain());
    return attribute_to_writer(&curves, changed_tags(), domain_size, *attr);
  };
  fn.remove = [](void *owner, const StringRef name) -> bool {
    Drawing &drawing = *static_cast<Drawing *>(owner);
    CurvesGeometry &curves = drawing.as_curves_for_write();

    if (try_delete_vertex_group(
            curves.vertex_group_names, name, [&]() { return curves.deform_verts_for_write(); }))
    {
      return true;
    }

    AttributeStorage &storage = curves.attribute_storage.wrap();
    if (const AttrBuiltinInfo *info = builtin_attributes().lookup_ptr(name)) {
      if (!info->deletable) {
        return false;
      }
    }
    const std::optional<AttrUpdateOnChange> fn = changed_tags().lookup_try(name);
    const bool removed = storage.remove(name);
    if (!removed) {
      return false;
    }
    if (fn) {
      (*fn)(owner);
    }
    return true;
  };
  fn.add = [](void *owner,
              const StringRef name,
              const AttrDomain domain,
              const AttrType type,
              const AttributeInit &initializer) {
    Drawing &drawing = *static_cast<Drawing *>(owner);
    CurvesGeometry &curves = drawing.as_curves_for_write();
    const int domain_size = get_domain_size(owner, domain);
    AttributeStorage &storage = curves.attribute_storage.wrap();
    if (const AttrBuiltinInfo *info = builtin_attributes().lookup_ptr(name)) {
      if (info->domain != domain || info->type != type) {
        return false;
      }
    }
    if (storage.lookup(name)) {
      return false;
    }
    const bool array = array_storage_required().contains(name);
    Attribute::DataVariant data = attribute_init_to_data(type, domain_size, initializer, array);
    storage.add(name, domain, type, std::move(data));
    if (initializer.type != AttributeInit::Type::Construct) {
      if (const std::optional<AttrUpdateOnChange> fn = changed_tags().lookup_try(name)) {
        (*fn)(owner);
      }
    }
    return true;
  };
  fn.rename = [](void *owner, const Map<StringRef, StringRef> &name_map, bool overwrite) {
    Drawing &drawing = *static_cast<Drawing *>(owner);
    CurvesGeometry &curves = drawing.as_curves_for_write();
    return rename_attributes(
        curves.attribute_storage.wrap(),
        name_map,
        overwrite,
        builtin_attributes(),
        array_storage_required(),
        [&](const bke::AttrDomain domain) { return get_domain_size(owner, domain); },
        &curves.vertex_group_names,
        [&]() { return curves.deform_verts_for_write(); });
  };
  fn.assign_data = [](void *owner, StringRef name, const AttributeInit &initializer) {
    Drawing &drawing = *static_cast<Drawing *>(owner);
    CurvesGeometry &curves = drawing.as_curves_for_write();
    AttributeStorage &storage = curves.attribute_storage.wrap();
    Attribute *attr = storage.lookup(name);
    if (!attr) {
      return false;
    }
    Attribute::DataVariant data = attribute_init_to_data(attr->data_type(),
                                                         get_domain_size(owner, attr->domain()),
                                                         initializer,
                                                         array_storage_required().contains(name));
    attr->assign_data(std::move(data));
    if (initializer.type != AttributeInit::Type::Construct) {
      if (const std::optional<AttrUpdateOnChange> fn = changed_tags().lookup_try(name)) {
        (*fn)(owner);
      }
    }
    return true;
  };

  return fn;
}

const AttributeAccessorFunctions &get_attribute_accessor_functions()
{
  static const AttributeAccessorFunctions fn = get_grease_pencil_drawing_accessor_functions();
  return fn;
}

}  // namespace drawing

static const auto &changed_tags()
{
  static Map<StringRef, AttrUpdateOnChange> attributes;
  return attributes;
}

static const auto &builtin_attributes()
{
  static auto attributes = []() {
    Map<StringRef, AttrBuiltinInfo> map;
    return map;
  }();
  return attributes;
}

static const auto &array_storage_required()
{
  static Set<StringRef> attributes{};
  return attributes;
}

static int get_domain_size(const void *owner, const AttrDomain domain)
{
  const GreasePencil &grease_pencil = *static_cast<const GreasePencil *>(owner);
  return domain == AttrDomain::Layer ? grease_pencil.layers().size() : 0;
}

static AttributeAccessorFunctions get_grease_pencil_accessor_functions()
{
  AttributeAccessorFunctions fn{};
  fn.domain_supported = [](const void * /*owner*/, const AttrDomain domain) {
    return domain == AttrDomain::Layer;
  };
  fn.domain_size = get_domain_size;
  fn.builtin_domain_and_type = [](const void * /*owner*/, const StringRef /*name*/)
      -> std::optional<AttributeDomainAndType> { return std::nullopt; };
  fn.lookup = [](const void *owner, const StringRef name) -> GAttributeReader {
    const GreasePencil &grease_pencil = *static_cast<const GreasePencil *>(owner);
    const AttributeStorage &storage = grease_pencil.attribute_storage.wrap();
    const Attribute *attribute = storage.lookup(name);
    if (!attribute) {
      return {};
    }
    const int domain_size = get_domain_size(owner, AttrDomain::Layer);
    return attribute_to_reader(*attribute, AttrDomain::Layer, domain_size);
  };
  fn.get_builtin_default = [](const void * /*owner*/, StringRef name) -> GPointer {
    const AttrBuiltinInfo &info = builtin_attributes().lookup(name);
    return info.default_value;
  };
  fn.lookup_meta_data = [](const void *owner, StringRef name) -> std::optional<AttributeMetaData> {
    const GreasePencil &grease_pencil = *static_cast<const GreasePencil *>(owner);
    const AttributeStorage &storage = grease_pencil.attribute_storage.wrap();
    const Attribute *attr = storage.lookup(name);
    if (!attr) {
      return std::nullopt;
    }
    return AttributeMetaData{attr->domain(), attr->data_type()};
  };
  fn.adapt_domain = [](const void * /*owner*/,
                       const GVArray &varray,
                       const AttrDomain from_domain,
                       const AttrDomain to_domain) {
    if (from_domain == to_domain && from_domain == AttrDomain::Layer) {
      return varray;
    }
    return GVArray{};
  };
  fn.foreach_attribute = [](const void *owner,
                            const FunctionRef<void(const AttributeIter &)> fn,
                            const AttributeAccessor &accessor) {
    const GreasePencil &grease_pencil = *static_cast<const GreasePencil *>(owner);
    const AttributeStorage &storage = grease_pencil.attribute_storage.wrap();
    for (const Attribute &attribute : storage) {
      const auto get_fn = [&]() {
        const int domain_size = get_domain_size(owner, AttrDomain::Layer);
        return attribute_to_reader(attribute, AttrDomain::Layer, domain_size);
      };
      AttributeIter iter(attribute.name(), attribute.domain(), attribute.data_type(), get_fn);
      iter.is_builtin = builtin_attributes().contains(attribute.name());
      iter.storage_type = attribute.storage_type();
      iter.accessor = &accessor;
      fn(iter);
      if (iter.is_stopped()) {
        break;
      }
    }
  };
  fn.lookup_validator = [](const void * /*owner*/, const StringRef name) -> AttributeValidator {
    const AttrBuiltinInfo *info = builtin_attributes().lookup_ptr(name);
    if (!info) {
      return {};
    }
    return info->validator;
  };
  fn.lookup_for_write = [](void *owner, const StringRef name) -> GAttributeWriter {
    GreasePencil &grease_pencil = *static_cast<GreasePencil *>(owner);
    AttributeStorage &storage = grease_pencil.attribute_storage.wrap();
    Attribute *attribute = storage.lookup(name);
    if (!attribute) {
      return {};
    }
    const int domain_size = get_domain_size(owner, AttrDomain::Layer);
    return attribute_to_writer(&grease_pencil, {}, domain_size, *attribute);
  };
  fn.remove = [](void *owner, const StringRef name) -> bool {
    GreasePencil &grease_pencil = *static_cast<GreasePencil *>(owner);
    AttributeStorage &storage = grease_pencil.attribute_storage.wrap();
    if (const AttrBuiltinInfo *info = builtin_attributes().lookup_ptr(name)) {
      if (!info->deletable) {
        return false;
      }
    }
    const std::optional<AttrUpdateOnChange> fn = changed_tags().lookup_try(name);
    const bool removed = storage.remove(name);
    if (!removed) {
      return false;
    }
    if (fn) {
      (*fn)(owner);
    }
    return true;
  };
  fn.add = [](void *owner,
              const StringRef name,
              const AttrDomain domain,
              const bke::AttrType type,
              const AttributeInit &initializer) {
    GreasePencil &grease_pencil = *static_cast<GreasePencil *>(owner);
    const int domain_size = get_domain_size(owner, domain);
    AttributeStorage &storage = grease_pencil.attribute_storage.wrap();
    if (const AttrBuiltinInfo *info = builtin_attributes().lookup_ptr(name)) {
      if (info->domain != domain || info->type != type) {
        return false;
      }
    }
    if (storage.lookup(name)) {
      return false;
    }
    const bool array = array_storage_required().contains(name);
    Attribute::DataVariant data = attribute_init_to_data(type, domain_size, initializer, array);
    storage.add(name, domain, type, std::move(data));
    if (initializer.type != AttributeInit::Type::Construct) {
      if (const std::optional<AttrUpdateOnChange> fn = changed_tags().lookup_try(name)) {
        (*fn)(owner);
      }
    }
    return true;
  };
  fn.rename = [](void *owner,
                 const Map<StringRef, StringRef> &name_map,
                 bool overwrite) -> Set<StringRef> {
    GreasePencil &grease_pencil = *static_cast<GreasePencil *>(owner);
    return rename_attributes(
        grease_pencil.attribute_storage.wrap(),
        name_map,
        overwrite,
        builtin_attributes(),
        array_storage_required(),
        [&](const bke::AttrDomain domain) { return get_domain_size(owner, domain); },
        std::nullopt,
        {});
  };
  fn.assign_data = [](void *owner, StringRef name, const AttributeInit &initializer) {
    GreasePencil &grease_pencil = *static_cast<GreasePencil *>(owner);
    AttributeStorage &storage = grease_pencil.attribute_storage.wrap();
    Attribute *attr = storage.lookup(name);
    if (!attr) {
      return false;
    }
    Attribute::DataVariant data = attribute_init_to_data(attr->data_type(),
                                                         get_domain_size(owner, attr->domain()),
                                                         initializer,
                                                         array_storage_required().contains(name));
    attr->assign_data(std::move(data));
    if (initializer.type != AttributeInit::Type::Construct) {
      if (const std::optional<AttrUpdateOnChange> fn = changed_tags().lookup_try(name)) {
        (*fn)(owner);
      }
    }
    return true;
  };

  return fn;
}

const AttributeAccessorFunctions &get_attribute_accessor_functions()
{
  static const AttributeAccessorFunctions fn = get_grease_pencil_accessor_functions();
  return fn;
}

}  // namespace blender::bke::greasepencil
