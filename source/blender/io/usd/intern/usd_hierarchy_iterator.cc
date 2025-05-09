/* SPDX-FileCopyrightText: 2019 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "usd.hh"

#include "usd_armature_utils.hh"
#include "usd_blend_shape_utils.hh"
#include "usd_hierarchy_iterator.hh"
#include "usd_skel_convert.hh"
#include "usd_skel_root_utils.hh"
#include "usd_utils.hh"
#include "usd_writer_abstract.hh"
#include "usd_writer_armature.hh"
#include "usd_writer_camera.hh"
#include "usd_writer_curves.hh"
#include "usd_writer_hair.hh"
#include "usd_writer_light.hh"
#include "usd_writer_mesh.hh"
#include "usd_writer_metaball.hh"
#include "usd_writer_pointinstancer.hh"
#include "usd_writer_points.hh"
#include "usd_writer_text.hh"
#include "usd_writer_transform.hh"
#include "usd_writer_volume.hh"

#include <string>

#include "BKE_main.hh"
#include "BKE_report.hh"

#include "BLI_assert.h"

#include "DNA_layer_types.h"
#include "DNA_object_types.h"

namespace blender::io::usd {

USDHierarchyIterator::USDHierarchyIterator(Main *bmain,
                                           Depsgraph *depsgraph,
                                           pxr::UsdStageRefPtr stage,
                                           const USDExportParams &params)
    : AbstractHierarchyIterator(bmain, depsgraph), stage_(stage), params_(params)
{
}

bool USDHierarchyIterator::mark_as_weak_export(const Object *object) const
{
  if (params_.selected_objects_only && (object->base_flag & BASE_SELECTED) == 0) {
    return true;
  }

  switch (object->type) {
    case OB_EMPTY:
      /* Always assume empties are being exported intentionally. */
      return false;
    case OB_MESH:
    case OB_MBALL:
    case OB_FONT:
      return !params_.export_meshes;
    case OB_CAMERA:
      return !params_.export_cameras;
    case OB_LAMP:
      return !params_.export_lights;
    case OB_CURVES_LEGACY:
    case OB_CURVES:
      return !params_.export_curves;
    case OB_VOLUME:
      return !params_.export_volumes;
    case OB_ARMATURE:
      return !params_.export_armatures;
    case OB_POINTCLOUD:
      return !params_.export_points;

    default:
      /* Assume weak for all other types. */
      return true;
  }
}

void USDHierarchyIterator::release_writer(AbstractHierarchyWriter *writer)
{
  delete static_cast<USDAbstractWriter *>(writer);
}

std::string USDHierarchyIterator::make_valid_name(const std::string &name) const
{
  return make_safe_name(name, params_.allow_unicode);
}

void USDHierarchyIterator::process_usd_skel() const
{
  skel_export_chaser(stage_,
                     armature_export_map_,
                     skinned_mesh_export_map_,
                     shape_key_mesh_export_map_,
                     depsgraph_);

  create_skel_roots(stage_, params_);
}

void USDHierarchyIterator::set_export_frame(float frame_nr)
{
  /* The USD stage is already set up to have FPS time-codes per frame. */
  export_time_ = pxr::UsdTimeCode(frame_nr);
}

USDExporterContext USDHierarchyIterator::create_usd_export_context(const HierarchyContext *context)
{
  pxr::SdfPath path;
  if (params_.root_prim_path[0] != '\0') {
    path = pxr::SdfPath(params_.root_prim_path + context->export_path);
  }
  else {
    path = pxr::SdfPath(context->export_path);
  }

  if (params_.merge_parent_xform && context->is_object_data_context && !context->is_parent) {
    bool can_merge_with_xform = true;
    if (params_.export_shapekeys && is_mesh_with_shape_keys(context->object)) {
      can_merge_with_xform = false;
    }

    if (params_.use_instancing && (context->is_prototype() || context->is_instance())) {
      can_merge_with_xform = false;
    }

    if (can_merge_with_xform) {
      path = path.GetParentPath();
    }
  }

  /* Returns the same path that was passed to `stage_` object during it's creation (via
   * `pxr::UsdStage::CreateNew` function). */
  const pxr::SdfLayerHandle root_layer = stage_->GetRootLayer();
  const std::string export_file_path = root_layer->GetRealPath();
  auto get_time_code = [this]() { return this->export_time_; };

  return USDExporterContext{
      bmain_, depsgraph_, stage_, path, get_time_code, params_, export_file_path};
}

void USDHierarchyIterator::determine_point_instancers(const HierarchyContext *context)
{
  if (!context) {
    return;
  }

  if (context->object->type == OB_ARMATURE) {
    return;
  }

  if (context->is_point_instancer()) {
    /* Mark the point instancer's children as a point instance.*/
    USDExporterContext usd_export_context = create_usd_export_context(context);
    ExportChildren *children = graph_children(context);

    bool is_referencing_self = false;

    std::string instancer_path_str;
    if (strlen(params_.root_prim_path) != 0) {
      instancer_path_str = std::string(params_.root_prim_path) + context->export_path;
    }
    else {
      instancer_path_str = context->export_path;
    }

    if (children != nullptr) {
      for (HierarchyContext *child_context : *children) {
        if (child_context->is_instance() && child_context->duplicator != nullptr &&
            !child_context->original_export_path.empty())
        {
          if (isSubPath(context->export_path, child_context->original_export_path)) {
            is_referencing_self = true;
            break;
          }

          if (strlen(params_.root_prim_path) != 0) {
            std::string proto_path_str = std::string(params_.root_prim_path) +
                                         child_context->original_export_path;
            prototype_paths[instancer_path_str].insert(
                std::make_pair(proto_path_str, child_context->object));
          }
          else {
            prototype_paths[instancer_path_str].insert(
                std::make_pair(child_context->original_export_path, child_context->object));
          }
          child_context->is_point_instance = true;
        }
        else {
          child_context->is_point_proto = true;
        }
      }
    }

    if (is_referencing_self) {
      /* MARK: If the "Instance on Points" node uses an Object as a prototype,
       but the "Object Info" node has not enabled the "As Instance" option,
       then the generated reference path is incorrect and refers to itself. */
      if (is_referencing_self) {
        BKE_reportf(
            params_.worker_status->reports,
            RPT_WARNING,
            "One or more objects used as prototypes in 'Instance on Points' nodes either do not "
            "have 'As Instance' enabled in their 'Object Info' nodes, or the prototype is the "
            "base geometry input itself—both cases prevent valid point instancer export. If it's "
            "the former, enable 'As Instance' to avoid incorrect self-referencing.");
      }

      prototype_paths[instancer_path_str].clear();
      for (HierarchyContext *child_context : *children) {
        child_context->is_point_instance = false;
        child_context->is_point_proto = false;
      }
    }
  }
}

AbstractHierarchyWriter *USDHierarchyIterator::create_transform_writer(
    const HierarchyContext *context)
{
  /* transform writer is always called before data writers, so determin if the Xform's children is
   * a point instancer before writing data */
  determine_point_instancers(context);
  return new USDTransformWriter(create_usd_export_context(context));
}

AbstractHierarchyWriter *USDHierarchyIterator::create_data_writer(const HierarchyContext *context)
{
  USDExporterContext usd_export_context = create_usd_export_context(context);
  USDAbstractWriter *data_writer = nullptr;
  std::set<std::pair<std::string, Object *>> proto_paths =
      prototype_paths[usd_export_context.usd_path.GetParentPath().GetString()];

  switch (context->object->type) {
    case OB_MESH:
      if (usd_export_context.export_params.export_meshes) {
        if (context->is_point_instancer() && !proto_paths.empty()) {

          data_writer = new USDPointInstancerWriter(usd_export_context, proto_paths);

          /* Handle Mesh base data */
          USDExporterContext mesh_context = create_pi_base_path_context(context,
                                                                        usd_export_context);
          USDMeshWriter *mesh_writer = new USDMeshWriter(mesh_context);
          mesh_writer->write(const_cast<HierarchyContext &>(*context));
          if (mesh_writer && (params_.export_armatures || params_.export_shapekeys)) {
            add_usd_skel_export_mapping(context->object, mesh_writer->usd_path());
          }
        }
        else {
          data_writer = new USDMeshWriter(usd_export_context);
        }
      }
      else {
        return nullptr;
      }
      break;
    case OB_CAMERA:
      if (usd_export_context.export_params.export_cameras) {
        data_writer = new USDCameraWriter(usd_export_context);
      }
      else {
        return nullptr;
      }
      break;
    case OB_LAMP:
      if (usd_export_context.export_params.export_lights) {
        data_writer = new USDLightWriter(usd_export_context);
      }
      else {
        return nullptr;
      }
      break;
    case OB_MBALL:
      data_writer = new USDMetaballWriter(usd_export_context);
      break;
    case OB_FONT:
      data_writer = new USDTextWriter(usd_export_context);
      break;
    case OB_CURVES_LEGACY:
    case OB_CURVES:
      if (usd_export_context.export_params.export_curves) {
        if (context->is_point_instancer() && !proto_paths.empty()) {
          data_writer = new USDPointInstancerWriter(usd_export_context, proto_paths);

          /* Handle Curve base data */
          USDExporterContext curves_context = create_pi_base_path_context(context,
                                                                          usd_export_context);
          USDCurvesWriter *curves_writer = new USDCurvesWriter(curves_context);
          curves_writer->write(const_cast<HierarchyContext &>(*context));
          if (curves_writer && (params_.export_armatures || params_.export_shapekeys)) {
            add_usd_skel_export_mapping(context->object, curves_writer->usd_path());
          }
        }
        else {
          data_writer = new USDCurvesWriter(usd_export_context);
        }
      }
      else {
        return nullptr;
      }
      break;
    case OB_VOLUME:
      if (usd_export_context.export_params.export_volumes) {
        data_writer = new USDVolumeWriter(usd_export_context);
      }
      else {
        return nullptr;
      }
      break;
    case OB_ARMATURE:
      if (usd_export_context.export_params.export_armatures) {
        data_writer = new USDArmatureWriter(usd_export_context);
      }
      else {
        return nullptr;
      }
      break;
    case OB_POINTCLOUD:
      if (usd_export_context.export_params.export_points) {
        if (context->is_point_instancer() && !proto_paths.empty()) {
          data_writer = new USDPointInstancerWriter(usd_export_context, proto_paths);

          /* Handle Point Cloud base data */
          USDExporterContext pointcloud_context = create_pi_base_path_context(context,
                                                                              usd_export_context);
          USDPointsWriter *pointcloud_writer = new USDPointsWriter(pointcloud_context);
          pointcloud_writer->write(const_cast<HierarchyContext &>(*context));
          if (pointcloud_writer && (params_.export_armatures || params_.export_shapekeys)) {
            add_usd_skel_export_mapping(context->object, pointcloud_writer->usd_path());
          }
        }
        else {
          data_writer = new USDPointsWriter(usd_export_context);
        }
      }
      else {
        return nullptr;
      }
      break;

    case OB_EMPTY:
    case OB_SURF:
    case OB_SPEAKER:
    case OB_LIGHTPROBE:
    case OB_LATTICE:
    case OB_GREASE_PENCIL:
      return nullptr;

    case OB_TYPE_MAX:
      BLI_assert_msg(0, "OB_TYPE_MAX should not be used");
      return nullptr;
    default:
      BLI_assert_unreachable();
      return nullptr;
  }

  if (data_writer && !data_writer->is_supported(context)) {
    delete data_writer;
    return nullptr;
  }

  if (data_writer && (params_.export_armatures || params_.export_shapekeys)) {
    add_usd_skel_export_mapping(context->object, data_writer->usd_path());
  }

  return data_writer;
}

AbstractHierarchyWriter *USDHierarchyIterator::create_hair_writer(const HierarchyContext *context)
{
  if (!params_.export_hair) {
    return nullptr;
  }
  return new USDHairWriter(create_usd_export_context(context));
}

AbstractHierarchyWriter *USDHierarchyIterator::create_particle_writer(
    const HierarchyContext * /*context*/)
{
  return nullptr;
}

bool USDHierarchyIterator::include_data_writers(const HierarchyContext *context) const
{
  /* Don't generate data writers for instances. */

  return !(params_.use_instancing && context->is_instance());
}

bool USDHierarchyIterator::include_child_writers(const HierarchyContext *context) const
{
  /* Don't generate writers for children of instances. */

  return !(params_.use_instancing && context->is_instance());
}

void USDHierarchyIterator::add_usd_skel_export_mapping(const Object *obj, const pxr::SdfPath &path)
{
  if (params_.export_shapekeys && is_mesh_with_shape_keys(obj)) {
    shape_key_mesh_export_map_.add(obj, path);
  }

  if (params_.export_armatures && obj->type == OB_ARMATURE) {
    armature_export_map_.add(obj, path);
  }

  if (params_.export_armatures && obj->type == OB_MESH &&
      can_export_skinned_mesh(*obj, depsgraph_))
  {
    skinned_mesh_export_map_.add(obj, path);
  }
}

USDExporterContext USDHierarchyIterator::create_pi_base_path_context(
    const HierarchyContext *context, const USDExporterContext &usd_export_context)
{
  BLI_assert(context && context->object);

  std::string raw_name = context->object->id.name;

  /* Blender automatically prefixes object ID names with "OB" (e.g., "OBCube"). To get the actual
   * object name, we strip the "OB" prefix. */
  if (raw_name.rfind("OB", 0) == 0) {
    raw_name = raw_name.substr(2);
  }

  std::string base_name = raw_name + "_base";
  std::string safe_name = make_safe_name(base_name,
                                         usd_export_context.export_params.allow_unicode);

  pxr::SdfPath base_path = usd_export_context.usd_path.GetParentPath().AppendChild(
      pxr::TfToken(safe_name));

  USDExporterContext new_context = usd_export_context;
  *const_cast<pxr::SdfPath *>(&new_context.usd_path) = base_path;

  return new_context;
}

}  // namespace blender::io::usd
