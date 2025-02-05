/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_geometry_set.hh"
#include "BKE_geometry_set_instances.hh"
#include "BKE_instances.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_material.hh"
#include "BKE_mesh.h"
#include "BKE_object.hh"
#include "BKE_pointcloud.hh"

#include "DEG_depsgraph_query.hh"

#include "DNA_collection_types.h"
#include "DNA_curves_types.h"
#include "DNA_mesh_types.h"
#include "DNA_pointcloud_types.h"

#include "ED_screen.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "BLI_map.hh"
#include "BLI_math_matrix.hh"

#include "object_intern.hh"

namespace blender::ed::object {

static void set_raw_object_transform(Object &ob, const float4x4 &transform)
{
  float3 location;
  math::EulerXYZ rotation;
  float3 scale;
  math::to_loc_rot_scale_safe<true>(transform, location, rotation, scale);
  copy_v3_v3(ob.loc, location);
  copy_v3_v3(ob.rot, float3(rotation.x().radian(), rotation.y().radian(), rotation.z().radian()));
  copy_v3_v3(ob.scale, scale);
}

static void transform_raw_object_transform(Object &ob, const float4x4 &transform)
{
  const float4x4 old_transform = math::from_loc_rot_scale<float4x4>(
      ob.loc, math::EulerXYZ(float3(ob.rot)), float3(ob.scale));
  const float4x4 new_transform = transform * old_transform;
  set_raw_object_transform(ob, new_transform);
}

struct ComponentObjects {
  Object *mesh_ob = nullptr;
  Object *curves_ob = nullptr;
  Object *pointcloud_ob = nullptr;
  Vector<Object *> instance_objects;
};

class GeometryToEditableOp {
 private:
  Main &bmain_;
  Map<const ID *, Object *> new_object_by_generated_geometry_;
  Map<bke::InstanceReference, Collection *> collection_by_instance_;

 public:
  GeometryToEditableOp(Main &bmain) : bmain_(bmain) {}

  Collection *build_collection_for_geometry(const Object &src_ob_eval,
                                            const bke::GeometrySet &geometry)
  {
    ComponentObjects component_objects = this->get_objects_for_geometry(src_ob_eval, geometry);
    return this->flat_collection_from_geometry_set_objects(
        component_objects, geometry.name.empty() ? BKE_id_name(src_ob_eval.id) : geometry.name);
  }

 private:
  Collection *flat_collection_from_geometry_set_objects(const ComponentObjects &component_objects,
                                                        const StringRefNull name)
  {
    Collection *collection = BKE_collection_add(&bmain_, nullptr, name.c_str());
    if (component_objects.mesh_ob != nullptr) {
      BKE_collection_object_add(&bmain_, collection, component_objects.mesh_ob);
    }
    if (component_objects.curves_ob != nullptr) {
      BKE_collection_object_add(&bmain_, collection, component_objects.curves_ob);
    }
    if (component_objects.pointcloud_ob != nullptr) {
      BKE_collection_object_add(&bmain_, collection, component_objects.pointcloud_ob);
    }
    for (Object *instance_object : component_objects.instance_objects) {
      BKE_collection_object_add(&bmain_, collection, instance_object);
    }
    return collection;
  }

  ComponentObjects get_objects_for_geometry(const Object &src_ob_eval,
                                            const bke::GeometrySet &geometry)
  {
    ComponentObjects objects;
    if (const Mesh *mesh = geometry.get_mesh()) {
      if (mesh->verts_num > 0) {
        objects.mesh_ob = this->get_or_create_object_for_mesh(src_ob_eval, *mesh, geometry.name);
      }
    }
    if (const Curves *curves = geometry.get_curves()) {
      if (curves->geometry.curve_num > 0) {
        objects.curves_ob = this->get_or_create_object_for_curves(
            src_ob_eval, *curves, geometry.name);
      }
    }
    if (const PointCloud *pointcloud = geometry.get_pointcloud()) {
      if (pointcloud->totpoint > 0) {
        objects.pointcloud_ob = this->get_or_create_object_for_pointcloud(
            src_ob_eval, *pointcloud, geometry.name);
      }
    }
    if (const bke::Instances *instances = geometry.get_instances()) {
      objects.instance_objects = this->create_objects_for_instances(src_ob_eval, *instances);
    }
    return objects;
  }

  Object *get_or_create_object_for_mesh(const Object & /*src_ob_eval*/,
                                        const Mesh &src_mesh,
                                        const StringRefNull name)
  {
    return new_object_by_generated_geometry_.lookup_or_add_cb(&src_mesh.id, [&]() {
      Mesh *new_mesh = reinterpret_cast<Mesh *>(BKE_id_new(&bmain_, ID_ME, name.c_str()));
      Object *new_ob = BKE_object_add_only_object(&bmain_, OB_MESH, name.c_str());
      new_ob->data = new_mesh;

      Mesh *mesh_to_move_from = BKE_mesh_copy_for_eval(src_mesh);
      BKE_mesh_nomain_to_mesh(mesh_to_move_from, new_mesh, new_ob);
      new_mesh->attributes_for_write().remove_anonymous();
      /* TODO: Materials. */
      /* TODO: #remove_invalid_attribute_strings */
      /* TODO: #multires_customdata_delete */
      return new_ob;
    });
  }

  Object *get_or_create_object_for_curves(const Object & /*src_ob_eval*/,
                                          const Curves &src_curves,
                                          const StringRefNull name)
  {
    return new_object_by_generated_geometry_.lookup_or_add_cb(&src_curves.id, [&]() {
      Curves *new_curves = reinterpret_cast<Curves *>(BKE_id_new(&bmain_, ID_CV, name.c_str()));
      Object *new_ob = BKE_object_add_only_object(&bmain_, OB_CURVES, name.c_str());
      new_ob->data = new_curves;

      new_curves->geometry.wrap() = src_curves.geometry.wrap();
      new_curves->geometry.wrap().attributes_for_write().remove_anonymous();
      /* TODO: Materials. */
      return new_ob;
    });
  }

  Object *get_or_create_object_for_pointcloud(const Object & /*src_ob_eval*/,
                                              const PointCloud &src_pointcloud,
                                              const StringRefNull name)
  {
    return new_object_by_generated_geometry_.lookup_or_add_cb(&src_pointcloud.id, [&]() {
      PointCloud *new_pointcloud = reinterpret_cast<PointCloud *>(
          BKE_id_new(&bmain_, ID_PT, name.c_str()));
      Object *new_ob = BKE_object_add_only_object(&bmain_, OB_POINTCLOUD, name.c_str());
      new_ob->data = new_pointcloud;

      PointCloud *pointcloud_to_move_from = BKE_pointcloud_copy_for_eval(&src_pointcloud);
      BKE_pointcloud_nomain_to_pointcloud(pointcloud_to_move_from, new_pointcloud);
      /* TODO: Materials. */
      return new_ob;
    });
  }

  Vector<Object *> create_objects_for_instances(const Object &src_ob_eval,
                                                const bke::Instances &src_instances)
  {
    bke::Instances instances = src_instances;
    instances.remove_unused_references();

    Vector<Collection *> collection_by_handle;
    for (const bke::InstanceReference &reference : instances.references()) {
      collection_by_handle.append(
          this->get_or_create_collection_for_instance_reference(src_ob_eval, reference));
    }

    const Span<int> handles = instances.reference_handles();
    const Span<float4x4> transforms = instances.transforms();

    Vector<Object *> objects;
    for (const int instance_i : IndexRange(instances.instances_num())) {
      const int handle = handles[instance_i];
      if (handle < 0 || handle >= collection_by_handle.size()) {
        continue;
      }
      Collection *collection_to_instance = collection_by_handle[handle];
      if (!collection_to_instance) {
        continue;
      }
      Object *instance_object = BKE_object_add_only_object(
          &bmain_, OB_EMPTY, BKE_id_name(collection_to_instance->id));
      instance_object->transflag = OB_DUPLICOLLECTION;
      instance_object->instance_collection = collection_to_instance;

      const float4x4 &transform = transforms[instance_i];
      set_raw_object_transform(*instance_object, transform);

      objects.append(instance_object);
    }
    return objects;
  }

  Collection *get_or_create_collection_for_instance_reference(
      const Object &src_ob_eval, const bke::InstanceReference &reference)
  {
    if (Collection *collection = collection_by_instance_.lookup_default(reference, nullptr)) {
      return collection;
    }
    Collection *collection_for_reference = nullptr;
    switch (reference.type()) {
      case bke::InstanceReference::Type::None: {
        break;
      }
      case bke::InstanceReference::Type::Object: {
        /* Create a collection for the object because we can't instance objects directly. */
        Object &object_eval = reference.object();
        collection_for_reference = BKE_collection_add(
            &bmain_, nullptr, BKE_id_name(object_eval.id));
        Object *object_orig = DEG_get_original_object(&object_eval);
        BKE_collection_object_add(&bmain_, collection_for_reference, object_orig);
        copy_v3_v3(collection_for_reference->instance_offset, object_orig->loc);
        break;
      }
      case bke::InstanceReference::Type::Collection: {
        collection_for_reference = &reference.collection();
        break;
      }
      case bke::InstanceReference::Type::GeometrySet: {
        collection_for_reference = this->build_collection_for_geometry(src_ob_eval,
                                                                       reference.geometry_set());
        break;
      }
    }
    collection_by_instance_.add(reference, collection_for_reference);
    return collection_for_reference;
  }
};

static int visual_geometry_to_editable_exec(bContext *C, wmOperator * /*op*/)
{
  Main &bmain = *CTX_data_main(C);
  Scene &scene = *CTX_data_scene(C);
  Depsgraph &depsgraph = *CTX_data_ensure_evaluated_depsgraph(C);
  ViewLayer &view_layer = *CTX_data_view_layer(C);
  LayerCollection &layer_collection = *BKE_layer_collection_get_active(&view_layer);

  Object *src_ob_orig = CTX_data_active_object(C);
  Object *src_ob_eval = DEG_get_evaluated_object(&depsgraph, src_ob_orig);
  if (!src_ob_eval) {
    return OPERATOR_CANCELLED;
  }

  GeometryToEditableOp op(bmain);

  bke::GeometrySet geometry_eval = bke::object_get_evaluated_geometry_set(*src_ob_eval);

  Collection *new_collection = op.build_collection_for_geometry(*src_ob_eval, geometry_eval);

  FOREACH_COLLECTION_OBJECT_RECURSIVE_BEGIN (new_collection, ob) {
    transform_raw_object_transform(*ob, src_ob_eval->object_to_world());
  }
  FOREACH_COLLECTION_OBJECT_RECURSIVE_END;

  BKE_collection_child_add(&bmain, layer_collection.collection, new_collection);
  BKE_view_layer_synced_ensure(&scene, &view_layer);

  BKE_view_layer_base_deselect_all(&scene, &view_layer);
  BKE_collection_objects_select(&scene, &view_layer, new_collection, false);

  LayerCollection *new_layer_collection = BKE_layer_collection_first_from_scene_collection(
      &view_layer, new_collection);
  BKE_layer_collection_activate(&view_layer, new_layer_collection);

  DEG_relations_tag_update(&bmain);
  WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, &scene);
  WM_main_add_notifier(NC_OBJECT | ND_DRAW, nullptr);
  return OPERATOR_FINISHED;
}

void OBJECT_OT_visual_geometry_to_editable(wmOperatorType *ot)
{
  ot->name = "Visual Geometry to Editable";
  ot->description = "Convert geometry and instances into editable objects and collections";
  ot->idname = "OBJECT_OT_visual_geometry_to_editable";

  ot->exec = visual_geometry_to_editable_exec;
  ot->poll = ED_operator_object_active;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

}  // namespace blender::ed::object
