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
#include "DNA_curves_types.h"
#include "DNA_mesh_types.h"

#include "DNA_pointcloud_types.h"
#include "ED_screen.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "BLI_map.hh"

#include "object_intern.hh"

namespace blender::ed::object {

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

  Object *build_object_for_geometry(const Object &src_ob_eval, const bke::GeometrySet &geometry)
  {
    Object *mesh_ob = nullptr;
    Object *curves_ob = nullptr;
    if (const Mesh *mesh = geometry.get_mesh()) {
      if (mesh->verts_num > 0) {
        mesh_ob = this->get_or_create_object_for_mesh(src_ob_eval, *mesh, geometry.name);
      }
    }
    if (const Curves *curves = geometry.get_curves()) {
      if (curves->geometry.curve_num > 0) {
        curves_ob = this->get_or_create_object_for_curves(src_ob_eval, *curves, geometry.name);
      }
    }

    const int num_objects = (mesh_ob != nullptr) + (curves_ob != nullptr);
    if (num_objects == 0) {
      return nullptr;
    }
    if (num_objects == 1) {
      if (mesh_ob != nullptr) {
        return mesh_ob;
      }
      if (curves_ob != nullptr) {
        return curves_ob;
      }
    }
    // TODO: create collection and instance that
    return mesh_ob;
  }

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
}

}  // namespace blender::ed::object
