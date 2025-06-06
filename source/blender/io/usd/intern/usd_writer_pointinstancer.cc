/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "usd_writer_pointinstancer.hh"
#include "usd_attribute_utils.hh"
#include "usd_utils.hh"
#include "usd_writer_curves.hh"
#include "usd_writer_mesh.hh"
#include "usd_writer_points.hh"

#include "BKE_anonymous_attribute_id.hh"
#include "BKE_collection.hh"
#include "BKE_geometry_set.hh"
#include "BKE_geometry_set_instances.hh"
#include "BKE_instances.hh"
#include "BKE_lib_id.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_object.hh"
#include "BKE_report.hh"

#include "BLI_math_euler.hh"
#include "BLI_math_matrix.hh"

#include "DNA_collection_types.h"
#include "DNA_layer_types.h"
#include "DNA_mesh_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_pointcloud_types.h"

#include <pxr/base/gf/math.h>
#include <pxr/base/gf/matrix3f.h>
#include <pxr/base/gf/quatd.h>
#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/range3f.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/scope.h>
#include <pxr/usd/usdGeom/xform.h>

#include "DEG_depsgraph_query.hh"
#include "IO_abstract_hierarchy_iterator.h"
#include <fmt/format.h>

namespace blender::io::usd {

USDPointInstancerWriter::USDPointInstancerWriter(
    const USDExporterContext &ctx, std::set<std::pair<pxr::SdfPath, Object *>> &paths)
    : USDAbstractWriter(ctx)
{
  proto_paths = paths;
}

void USDPointInstancerWriter::do_write(HierarchyContext &context)
{
  const pxr::UsdStageRefPtr stage = usd_export_context_.stage;
  Object *object_eval = context.object;
  bke::GeometrySet instance_geometry_set = bke::object_get_evaluated_geometry_set(*object_eval);

  const bke::GeometryComponent *component = instance_geometry_set.get_component(
      bke::GeometryComponent::Type::Instance);

  const bke::Instances *instances = static_cast<const bke::InstancesComponent &>(*component).get();

  int instance_num = instances->instances_num();
  const pxr::SdfPath &usd_path = usd_export_context_.usd_path;
  const pxr::UsdGeomPointInstancer usd_instancer = pxr::UsdGeomPointInstancer::Define(stage,
                                                                                      usd_path);
  const pxr::UsdTimeCode timecode = get_export_time_code();

  Span<float4x4> transforms = instances->transforms();
  BLI_assert(transforms.size() >= instance_num);

  if (transforms.size() != instance_num) {
    BKE_reportf(this->reports(),
                RPT_ERROR,
                "Instances number '%d' doesn't match transforms size '%lld'",
                instance_num,
                transforms.size());
    return;
  }

  /* evaluated positions */
  pxr::UsdAttribute position_attr = usd_instancer.CreatePositionsAttr();
  pxr::VtArray<pxr::GfVec3f> positions(instance_num);
  for (int i = 0; i < instance_num; i++) {
    const float3 &pos = transforms[i].location();
    positions[i] = pxr::GfVec3f(pos.x, pos.y, pos.z);
  }
  blender::io::usd::set_attribute(position_attr, positions, timecode, usd_value_writer_);

  /* orientations */
  pxr::UsdAttribute orientations_attr = usd_instancer.CreateOrientationsAttr();
  pxr::VtArray<pxr::GfQuath> orientation(instance_num);
  for (int i = 0; i < instance_num; i++) {
    const float3 euler = float3(math::to_euler(math::normalize(transforms[i])));
    const math::Quaternion quat = math::to_quaternion(math::EulerXYZ(euler));
    orientation[i] = pxr::GfQuath(quat.w, pxr::GfVec3h(quat.x, quat.y, quat.z));
  }
  orientations_attr.Set(orientation, timecode);

  /* scales */
  pxr::UsdAttribute scales_attr = usd_instancer.CreateScalesAttr();
  pxr::VtArray<pxr::GfVec3f> scales(instance_num);
  for (int i = 0; i < instance_num; i++) {
    const MatBase<float, 4, 4> &mat = transforms[i];
    blender::float3 scale_vec = math::to_scale<true>(mat);
    scales[i] = pxr::GfVec3f(scale_vec.x, scale_vec.y, scale_vec.z);
  }
  blender::io::usd::set_attribute(scales_attr, scales, timecode, usd_value_writer_);

  /* other attr */
  bke::AttributeAccessor attributes_eval = *component->attributes();
  attributes_eval.foreach_attribute([&](const bke::AttributeIter &iter) {
    if (iter.name[0] == '.' || blender::bke::attribute_name_is_anonymous(iter.name) ||
        ELEM(iter.name, "instance_transform") || ELEM(iter.name, "scale") ||
        ELEM(iter.name, "orientation") || ELEM(iter.name, "mask") ||
        ELEM(iter.name, "proto_index") || ELEM(iter.name, "id"))
    {
      return;
    }

    this->write_attribute_data(iter, usd_instancer, timecode);
  });

  /* prototypes relations */
  const pxr::SdfPath protoParentPath = usd_path.AppendChild(pxr::TfToken("Prototypes"));
  pxr::UsdPrim prototypesOver = stage->DefinePrim(protoParentPath);
  pxr::SdfPathVector new_proto_paths_str;

  std::map<std::string, int> proto_index_map;
  std::map<std::string, pxr::SdfPath> proto_path_map;

  if (!proto_paths.empty() && usd_instancer) {
    int iter = 0;

    for (const std::pair<pxr::SdfPath, Object *> &entry : proto_paths) {
      const pxr::SdfPath &source_path = entry.first;
      Object *obj = entry.second;

      if (source_path.IsEmpty()) {
        continue;
      }

      const pxr::SdfPath proto_path = protoParentPath.AppendChild(
          pxr::TfToken(proto_name + "_" + std::to_string(iter)));

      pxr::UsdPrim prim = stage->DefinePrim(proto_path);
      prim.GetReferences().AddReference(pxr::SdfReference("", source_path));
      new_proto_paths_str.push_back(proto_path);

      std::string ob_name = BKE_id_name(obj->id);
      proto_index_map[ob_name] = iter;
      proto_path_map[ob_name] = proto_path;

      ++iter;
    }
    usd_instancer.GetPrototypesRel().SetTargets(new_proto_paths_str);
    prototypesOver.GetPrim().SetSpecifier(pxr::SdfSpecifierOver);
    stage->GetRootLayer()->Save();
  }

  /* proto indices */
  /* must be the last to populate */
  pxr::UsdAttribute proto_indices_attr = usd_instancer.CreateProtoIndicesAttr();
  pxr::VtArray<int> proto_indices;

  Span<int> reference_handles = instances->reference_handles();
  Span<bke::InstanceReference> references = instances->references();

  for (int i = 0; i < instance_num; i++) {
    bke::InstanceReference reference = references[reference_handles[i]];

    switch (reference.type()) {
      case bke::InstanceReference::Type::Object: {
        Object &object = reference.object();
        std::string ob_name = BKE_id_name(object.id);

        if (proto_index_map.find(ob_name) != proto_index_map.end()) {
          proto_indices.push_back(proto_index_map[ob_name]);
        }
        break;
      }
      case bke::InstanceReference::Type::Collection: {
        Collection &collection = reference.collection();
        FOREACH_COLLECTION_OBJECT_RECURSIVE_BEGIN (&collection, object) {
          std::string ob_name = BKE_id_name(object->id);

          if (proto_index_map.find(ob_name) != proto_index_map.end()) {
            proto_indices.push_back(proto_index_map[ob_name]);
          }
        }
        FOREACH_COLLECTION_OBJECT_RECURSIVE_END;
        break;
      }
      case bke::InstanceReference::Type::GeometrySet: {
        bke::GeometrySet geometry_set = reference.geometry_set();
        std::string set_name = geometry_set.name;

        if (proto_index_map.find(set_name) != proto_index_map.end()) {
          proto_indices.push_back(proto_index_map[set_name]);

          Vector<const bke::GeometryComponent *> components = geometry_set.get_components();
          for (const bke::GeometryComponent *comp : components) {
            if (comp) {
              if (const bke::Instances *instances =
                      static_cast<const bke::InstancesComponent &>(*comp).get())
              {
                Span<float4x4> transforms = instances->transforms();
                if (transforms.size() == 1) {
                  if (proto_path_map.find(set_name) != proto_path_map.end()) {
                    // override position
                    const float3 &pos = transforms[0].location();
                    pxr::GfVec3d override_position = pxr::GfVec3d(pos.x, pos.y, pos.z);

                    // override rotation
                    const float3 euler = float3(math::to_euler(math::normalize(transforms[0])));
                    pxr::GfVec3f override_rotation = pxr::GfVec3f(euler.x, euler.y, euler.z);

                    // override scale
                    const float3 scale_vec = math::to_scale<true>(transforms[0]);
                    pxr::GfVec3f override_scale = pxr::GfVec3f(
                        scale_vec.x, scale_vec.y, scale_vec.z);

                    pxr::SdfPath proto_path = proto_path_map[set_name];
                    pxr::UsdPrim prim = stage->GetPrimAtPath(proto_path);

                    if (prim) {
                      pxr::UsdGeomXformable xformable(prim);
                      xformable.ClearXformOpOrder();

                      xformable.AddTranslateOp().Set(override_position);
                      xformable.AddRotateXYZOp().Set(override_rotation);
                      xformable.AddScaleOp().Set(override_scale);
                    }
                  }
                }
              }
            }
          }
        }
        break;
      }
      case bke::InstanceReference::Type::None: {
        break;
      }
    }
  }

  /* Handle Collection Prototypes */
  if (proto_indices.size() == 0) {
    handle_collection_prototypes(usd_instancer, timecode, instance_num);
  }
  else {
    blender::io::usd::set_attribute(
        proto_indices_attr, proto_indices, timecode, usd_value_writer_);
  }

  stage->GetRootLayer()->Save();
}

template<typename T>
static pxr::VtArray<T> DuplicateArray(const pxr::VtArray<T> &original, size_t copies)
{
  pxr::VtArray<T> newArray;
  size_t originalSize = original.size();
  newArray.resize(originalSize * copies);
  for (size_t i = 0; i < copies; ++i) {
    std::copy(original.begin(), original.end(), newArray.begin() + i * originalSize);
  }
  return newArray;
}

template<typename T, typename GetterFunc, typename CreatorFunc>
static void DuplicatePerInstanceAttribute(const GetterFunc &getter,
                                          const CreatorFunc &creator,
                                          size_t copies,
                                          const pxr::UsdTimeCode &timecode)
{
  pxr::VtArray<T> values;
  if (getter().Get(&values, timecode) && !values.empty()) {
    auto newValues = DuplicateArray(values, copies);
    creator().Set(newValues, timecode);
  }
}

void USDPointInstancerWriter::handle_collection_prototypes(
    const pxr::UsdGeomPointInstancer &usd_instancer,
    const pxr::UsdTimeCode timecode,
    int instance_num)
{
  // MARK: Handle Collection Prototypes
  // -----------------------------------------------------------------------------
  // In Blender, a Collection is not an actual Object type. When exporting, the iterator
  // flattens the Collection hierarchy, treating each object inside the Collection as an
  // individual prototype. However, all these prototypes share the same instance attributes
  // (e.g., positions, orientations, scales).
  //
  // To ensure correct arrangement, reading, and drawing in OpenUSD, we need to explicitly
  // duplicate the instance attributes across all prototypes derived from the Collection.
  pxr::VtArray<pxr::GfVec3f> positions;
  usd_instancer.GetPositionsAttr().Get(&positions, timecode);
  pxrBlender_v25_02__pxrReserved__::VtDictionary::size_type copies = proto_paths.size();

  ///* Duplicate Positions */
  if (usd_instancer.GetPositionsAttr() && usd_instancer.GetPositionsAttr().HasAuthoredValue()) {
    DuplicatePerInstanceAttribute<pxr::GfVec3f>(
        [&]() { return usd_instancer.GetPositionsAttr(); },
        [&]() { return usd_instancer.CreatePositionsAttr(); },
        copies,
        timecode);
  }

  ///* Duplicate Orientations */
  if (usd_instancer.GetOrientationsAttr() &&
      usd_instancer.GetOrientationsAttr().HasAuthoredValue())
  {
    DuplicatePerInstanceAttribute<pxr::GfQuath>(
        [&]() { return usd_instancer.GetOrientationsAttr(); },
        [&]() { return usd_instancer.CreateOrientationsAttr(); },
        copies,
        timecode);
  }

  ///* Duplicate Scales */
  if (usd_instancer.GetScalesAttr() && usd_instancer.GetScalesAttr().HasAuthoredValue()) {
    DuplicatePerInstanceAttribute<pxr::GfVec3f>([&]() { return usd_instancer.GetScalesAttr(); },
                                                [&]() { return usd_instancer.CreateScalesAttr(); },
                                                copies,
                                                timecode);
  }

  ///* Duplicate Velocities */
  if (usd_instancer.GetVelocitiesAttr() && usd_instancer.GetVelocitiesAttr().HasAuthoredValue()) {
    DuplicatePerInstanceAttribute<pxr::GfVec3f>(
        [&]() { return usd_instancer.GetVelocitiesAttr(); },
        [&]() { return usd_instancer.CreateVelocitiesAttr(); },
        copies,
        timecode);
  }

  ///* Duplicate AngularVelocities */
  if (usd_instancer.GetAngularVelocitiesAttr() &&
      usd_instancer.GetAngularVelocitiesAttr().HasAuthoredValue())
  {
    DuplicatePerInstanceAttribute<pxr::GfVec3f>(
        [&]() { return usd_instancer.GetAngularVelocitiesAttr(); },
        [&]() { return usd_instancer.CreateAngularVelocitiesAttr(); },
        copies,
        timecode);
  }

  ///* Duplicate Other Attributes (Primvars) */
  const pxr::UsdGeomPrimvarsAPI primvars_api(usd_instancer);
  std::vector<pxr::UsdGeomPrimvar> primvars = primvars_api.GetPrimvars();

  for (const pxr::UsdGeomPrimvar &primvar : primvars) {
    if (!primvar.HasAuthoredValue()) {
      continue;
    }

    const pxr::TfToken name = primvar.GetPrimvarName();
    const pxr::SdfValueTypeName type = primvar.GetTypeName();
    const pxr::TfToken interp = primvar.GetInterpolation();

    auto create = [&]() { return primvars_api.CreatePrimvar(name, type, interp); };

    /* Follow all types in blender::io::usd::convert_blender_type_to_usd */
    if (type == pxr::SdfValueTypeNames->FloatArray) {
      DuplicatePerInstanceAttribute<float>([&]() { return primvar; }, create, copies, timecode);
    }
    else if (type == pxr::SdfValueTypeNames->IntArray) {
      DuplicatePerInstanceAttribute<int>([&]() { return primvar; }, create, copies, timecode);
    }
    else if (type == pxr::SdfValueTypeNames->UCharArray) {
      DuplicatePerInstanceAttribute<unsigned char>(
          [&]() { return primvar; }, create, copies, timecode);
    }
    else if (type == pxr::SdfValueTypeNames->Float2Array) {
      DuplicatePerInstanceAttribute<pxr::GfVec2f>(
          [&]() { return primvar; }, create, copies, timecode);
    }
    else if (type == pxr::SdfValueTypeNames->Float3Array) {
      DuplicatePerInstanceAttribute<pxr::GfVec3f>(
          [&]() { return primvar; }, create, copies, timecode);
    }
    else if (type == pxr::SdfValueTypeNames->Color3fArray ||
             type == pxr::SdfValueTypeNames->Color4fArray)
    {
      DuplicatePerInstanceAttribute<pxr::GfVec4f>(
          [&]() { return primvar; }, create, copies, timecode);
    }
    else if (type == pxr::SdfValueTypeNames->QuatfArray) {
      DuplicatePerInstanceAttribute<pxr::GfQuatf>(
          [&]() { return primvar; }, create, copies, timecode);
    }
    else if (type == pxr::SdfValueTypeNames->BoolArray) {
      DuplicatePerInstanceAttribute<bool>([&]() { return primvar; }, create, copies, timecode);
    }
    else if (type == pxr::SdfValueTypeNames->StringArray) {
      DuplicatePerInstanceAttribute<std::string>(
          [&]() { return primvar; }, create, copies, timecode);
    }
  }

  // MARK: Ensure Instance Indices Exist
  // -----------------------------------------------------------------------------
  // If the PointInstancer has no authored instance indices, manually generate a default
  // sequence of indices to ensure the PointInstancer functions correctly in OpenUSD.
  // This guarantees that each instance can correctly reference its prototype.
  std::vector<int> index;
  for (int i = 0; i < proto_paths.size(); i++) {
    std::vector<int> current_proto_index(instance_num, i);
    index.insert(index.end(), current_proto_index.begin(), current_proto_index.end());
  }
  pxr::UsdAttribute proto_indices_attr = usd_instancer.GetProtoIndicesAttr();
  proto_indices_attr.Set(pxr::VtArray<int>(index.begin(), index.end()));
}

static std::optional<pxr::TfToken> convert_blender_domain_to_usd(
    const bke::AttrDomain blender_domain, std::string attr_name)
{
  switch (blender_domain) {
    case bke::AttrDomain::Instance:
      if (attr_name == "uv_map" || attr_name == "UVMap") {
        return pxr::UsdGeomTokens->faceVarying;
      }
    default:
      return std::nullopt;
  }
}

void USDPointInstancerWriter::write_attribute_data(const bke::AttributeIter &attr,
                                                   const pxr::UsdGeomPointInstancer &usd_instancer,
                                                   const pxr::UsdTimeCode timecode)
{
  const std::optional<pxr::TfToken> pv_interp = convert_blender_domain_to_usd(attr.domain,
                                                                              attr.name.c_str());
  const std::optional<pxr::SdfValueTypeName> pv_type = convert_blender_type_to_usd(attr.data_type);

  if (!pv_interp || !pv_type) {
    BKE_reportf(this->reports(),
                RPT_WARNING,
                "Attribute '%s' (Blender domain %d, type %d) cannot be converted to USD",
                attr.name.c_str(),
                int(attr.domain),
                attr.data_type);
    return;
  }

  const GVArray attribute = *attr.get();
  if (attribute.is_empty()) {
    return;
  }

  if (attr.name == "mask") {
    pxr::UsdAttribute idsAttr = usd_instancer.GetIdsAttr();
    if (!idsAttr) {
      idsAttr = usd_instancer.CreateIdsAttr();
    }

    pxr::UsdAttribute invisibleIdsAttr = usd_instancer.GetInvisibleIdsAttr();
    if (!invisibleIdsAttr) {
      invisibleIdsAttr = usd_instancer.CreateInvisibleIdsAttr();
    }

    const GVArray attribute = *attr.get();
    /// Retrieve mask values, store as int8_t to avoid std::vector<bool>.data() issues
    std::vector<int8_t> mask_values(attribute.size());
    attribute.materialize(IndexMask(attribute.size()), mask_values.data());

    pxr::VtArray<int64_t> ids;
    pxr::VtArray<int64_t> invisibleIds;
    ids.reserve(mask_values.size());

    for (int64_t i = 0; i < static_cast<int64_t>(mask_values.size()); ++i) {
      ids.push_back(i);
      if (mask_values[i] == 0) {
        invisibleIds.push_back(i);
      }
    }

    blender::io::usd::set_attribute(idsAttr, ids, timecode, usd_value_writer_);
    blender::io::usd::set_attribute(invisibleIdsAttr, invisibleIds, timecode, usd_value_writer_);
  }

  const pxr::TfToken pv_name(
      make_safe_name(attr.name, usd_export_context_.export_params.allow_unicode));
  const pxr::UsdGeomPrimvarsAPI pv_api = pxr::UsdGeomPrimvarsAPI(usd_instancer);

  pxr::UsdGeomPrimvar pv_attr = pv_api.CreatePrimvar(pv_name, *pv_type, *pv_interp);

  copy_blender_attribute_to_primvar(
      attribute, attr.data_type, timecode, pv_attr, usd_value_writer_);
}

}  // namespace blender::io::usd
