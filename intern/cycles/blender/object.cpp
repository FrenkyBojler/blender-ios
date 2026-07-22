/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "blender/light_linking.h"
#include "blender/object_cull.h"
#include "blender/sync.h"
#include "blender/util.h"

#include "scene/camera.h"
#include "scene/integrator.h"
#include "scene/light.h"
#include "scene/mesh.h"
#include "scene/object.h"
#include "scene/particles.h"
#include "scene/scene.h"
#include "scene/shader.h"
#include "scene/shader_graph.h"
#include "scene/shader_nodes.h"

#include "util/hash.h"
#include "util/log.h"
#include "util/task.h"
#include "util/time.h"

#include "BKE_attribute.hh"
#include "BKE_collection.hh"
#include "BKE_duplilist.hh"
#include "BKE_geometry_set.hh"
#include "BKE_idprop.hh"
#include "BKE_instances.hh"
#include "BKE_layer.hh"
#include "BKE_material.hh"
#include "BKE_object.hh"

#include "DEG_depsgraph_query.hh"

#include "RE_engine.h"

using blender::Object;

CCL_NAMESPACE_BEGIN

/* Utilities */

bool BlenderSync::BKE_object_is_modified(blender::Object &b_ob)
{
  /* test if we can instance or if the object is modified */
  if (b_ob.type == blender::OB_MBALL) {
    /* Multi-user and dupli meta-balls are fused, can't instance. */
    return true;
  }
  const int settings = preview ? blender::eModifierMode_Realtime : blender::eModifierMode_Render;
  if ((blender::BKE_object_is_modified(b_scene, &b_ob) & settings) != 0) {
    /* modifiers */
    return true;
  }

  /* Object level material links. Note the geometry material slot array may not match
   * the object matbits array, so we need to guard against out of bounds. */
  for (const int i : blender::IndexRange(BKE_object_material_count_eval(&b_ob))) {
    if (i < b_ob.totcol && b_ob.matbits && b_ob.matbits[i] != 0) {
      return true;
    }
  }

  return false;
}

bool BlenderSync::object_is_geometry(BObjectInfo &b_ob_info)
{
  blender::ID *b_ob_data = b_ob_info.object_data;

  if (!b_ob_data) {
    return false;
  }

  const blender::ObjectType type = b_ob_info.iter_object->type;

  if (type == blender::OB_VOLUME || type == blender::OB_CURVES || type == blender::OB_POINTCLOUD ||
      type == blender::OB_LAMP)
  {
    /* Will be exported as geometry. */
    return true;
  }

  return GS(b_ob_data->name) == blender::ID_ME;
}

bool BlenderSync::object_can_have_geometry(blender::Object &b_ob)
{
  const blender::ObjectType type = b_ob.type;
  switch (type) {
    case blender::OB_MESH:
    case blender::OB_CURVES_LEGACY:
    case blender::OB_SURF:
    case blender::OB_MBALL:
    case blender::OB_FONT:
    case blender::OB_CURVES:
    case blender::OB_POINTCLOUD:
    case blender::OB_VOLUME:
      /* TODO(weizhen): OB_LAMP */
      return true;
    default:
      return false;
  }
}

bool BlenderSync::object_is_light(blender::Object &b_ob)
{
  blender::ID *b_ob_data = object_get_data(b_ob, true);

  return (b_ob_data && GS(b_ob_data->name) == blender::ID_LA);
}

bool BlenderSync::object_is_camera(blender::Object &b_ob)
{
  blender::ID *b_ob_data = object_get_data(b_ob, true);

  return (b_ob_data && GS(b_ob_data->name) == blender::ID_CA);
}

void BlenderSync::sync_object_motion_init(blender::Object &b_parent,
                                          blender::Object &b_ob,
                                          Object *object)
{
  /* Initialize motion blur for object, detecting if it's enabled and creating motion
   * steps array if so. */
  array<Transform> motion = object->get_motion();

  Geometry *geom = object->get_geometry();
  if (!geom) {
    return;
  }

  int motion_steps = 0;
  bool use_motion_blur = false;

  const Scene::MotionType need_motion = scene->need_motion();
  if (need_motion == Scene::MOTION_BLUR) {
    motion_steps = object_motion_steps(b_parent, b_ob, Object::MAX_MOTION_STEPS);
    if (motion_steps && object_use_deform_motion(b_parent, b_ob)) {
      use_motion_blur = true;
    }
  }
  else if (need_motion != Scene::MOTION_NONE) {
    motion_steps = 3;
  }

  geom->set_use_motion_blur(use_motion_blur);

  motion.resize(motion_steps, transform_empty());

  if (motion_steps) {
    motion[motion_steps / 2] = object->get_tfm();

    /* update motion socket before trying to access object->motion_time */
    object->set_motion(motion);

    for (size_t step = 0; step < motion_steps; step++) {
      motion_times.insert(object->motion_time(step));
    }
  }
  else {
    object->set_motion(motion);
  }
}

Object *BlenderSync::sync_object(blender::ViewLayer &b_view_layer,
                                 blender::Object &b_ob,
                                 blender::DEGObjectIterData &b_deg_iter_data,
                                 const float motion_time,
                                 bool use_particle_hair,
                                 bool show_lights,
                                 BlenderObjectCulling &culling,
                                 TaskPool *geom_task_pool)
{
  const bool is_instance = b_deg_iter_data.dupli_object_current;
  blender::Object *b_parent = is_instance ? b_deg_iter_data.dupli_parent : &b_ob;
  blender::Object *b_real_object = is_instance ? b_deg_iter_data.dupli_object_current->ob : &b_ob;
  const bool use_adaptive_subdiv = object_subdivision_type(
                                       *b_real_object, preview, use_adaptive_subdivision) !=
                                   Mesh::SUBDIVISION_NONE;
  BObjectInfo b_ob_info{
      &b_ob, b_real_object, object_get_data(b_ob, use_adaptive_subdiv), use_adaptive_subdiv};
  const bool motion = motion_time != 0.0f;
  const Transform tfm = get_transform(b_ob.object_to_world());
  const int *persistent_id = nullptr;
  if (is_instance) {
    persistent_id = b_deg_iter_data.dupli_object_current->persistent_id;
    if (!motion && !b_ob_info.is_real_object_data()) {
      /* Remember which object data the geometry is coming from, so that we can sync it when the
       * object has changed. */
      instance_geometries_by_object[b_ob_info.real_object].insert(b_ob_info.object_data);
    }
  }

  /* only interested in object that we can create geometry from */
  if (!object_is_geometry(b_ob_info)) {
    return nullptr;
  }

  /* Perform object culling. */
  if (object_is_light(b_ob)) {
    if (!show_lights) {
      return nullptr;
    }
  }
  else if (culling.test(scene, b_ob, tfm)) {
    return nullptr;
  }

  /* Visibility flags for both parent and child. */
  blender::PointerRNA b_ob_rna_ptr = RNA_id_pointer_create(&b_ob.id);
  blender::PointerRNA cobject = RNA_pointer_get(&b_ob_rna_ptr, "cycles");
  /* Note base_parent is null for objects from the background scene. */
  const blender::Base *base_parent = BKE_view_layer_base_find(&b_view_layer, b_parent);
  const bool use_holdout = (base_parent && (base_parent->flag & blender::BASE_HOLDOUT) != 0) ||
                           ((b_parent->visibility_flag & blender::OB_HOLDOUT) != 0);
  PathRayVisibility visibility = object_ray_visibility(b_ob);

  if (b_parent != &b_ob) {
    visibility &= object_ray_visibility(*b_parent);
  }

  /* TODO: make holdout objects on excluded layer invisible for non-camera rays. */
#if 0
  if (use_holdout && (layer_flag & view_layer.exclude_layer)) {
    visibility &= ~(PATH_RAY_VISIBILITY_ALL & ~PATH_RAY_VISIBILITY_CAMERA);
  }
#endif

  /* Clear camera visibility for indirect only objects. */
  const bool use_indirect_only = !use_holdout && base_parent &&
                                 ((base_parent->flag & blender::BASE_INDIRECT_ONLY) != 0);
  if (use_indirect_only) {
    visibility &= ~PATH_RAY_VISIBILITY_CAMERA;
  }

  /* Don't export completely invisible objects. */
  if (visibility == PATH_RAY_VISIBILITY_NONE) {
    return nullptr;
  }

  /* Use task pool only for non-instances, since sync_dupli_particle accesses
   * geometry. This restriction should be removed for better performance. */
  TaskPool *object_geom_task_pool = (is_instance) ? nullptr : geom_task_pool;

  /* key to lookup object */
  const ObjectKey key(b_parent, persistent_id, b_ob_info.real_object, use_particle_hair);
  Object *object;

  /* motion vector case */
  if (motion) {
    object = object_map.find(key);

    if (object && object->use_motion()) {
      /* Set transform at matching motion time step. */
      const int time_index = object->motion_step(motion_time);
      if (time_index >= 0) {
        object->set_motion_tfm(tfm, time_index);
      }

      /* mesh deformation */
      if (object->get_geometry()) {
        sync_geometry_motion(
            b_ob_info, object, motion_time, use_particle_hair, object_geom_task_pool);
      }
    }

    return object;
  }

  /* test if we need to sync */
  bool object_updated = object_map.add_or_update(&object, &b_ob.id, &b_parent->id, key) ||
                        !object->tfm_equals(tfm);

  /* mesh sync */
  Geometry *geometry = sync_geometry(
      b_ob_info, object_updated, use_particle_hair, object_geom_task_pool);
  object->set_geometry(geometry);

  /* special case not tracked by object update flags */

  if (sync_object_attributes(b_ob, b_deg_iter_data, object)) {
    object_updated = true;
  }

  /* holdout */
  object->set_use_holdout(use_holdout);

  object->set_visibility(visibility);

  object->set_is_shadow_catcher((b_ob.visibility_flag & blender::OB_SHADOW_CATCHER) != 0 ||
                                (b_parent->visibility_flag & blender::OB_SHADOW_CATCHER) != 0);

  object->set_shadow_terminator_shading_offset(b_ob.shadow_terminator_shading_offset);

  object->set_shadow_terminator_geometry_offset(b_ob.shadow_terminator_geometry_offset);

  float ao_distance = get_float(cobject, "ao_distance");
  if (ao_distance == 0.0f && b_parent != &b_ob) {
    blender::PointerRNA b_parent_rna_ptr = RNA_id_pointer_create(&b_parent->id);
    blender::PointerRNA cparent = RNA_pointer_get(&b_parent_rna_ptr, "cycles");
    ao_distance = get_float(cparent, "ao_distance");
  }
  object->set_ao_distance(ao_distance);

  const bool is_caustics_caster = get_boolean(cobject, "is_caustics_caster");
  object->set_is_caustics_caster(is_caustics_caster);

  const bool is_caustics_receiver = get_boolean(cobject, "is_caustics_receiver");
  object->set_is_caustics_receiver(is_caustics_receiver);

  object->set_is_bake_target(b_ob_info.real_object == b_bake_target);

  /* sync the asset name for Cryptomatte */
  blender::Object *parent = b_ob.parent;
  ustring parent_name;
  if (parent) {
    while (parent->parent) {
      parent = parent->parent;
    }
    parent_name = BKE_id_name(parent->id);
  }
  else {
    parent_name = BKE_id_name(b_ob.id);
  }
  object->set_asset_name(parent_name);

  /* object sync
   * transform comparison should not be needed, but duplis don't work perfect
   * in the depsgraph and may not signal changes, so this is a workaround */
  const bool do_sync = object->is_modified() || object_updated ||
                       (object->get_geometry() && object->get_geometry()->is_modified());
  if (do_sync) {
    object->name = BKE_id_name(b_ob.id);
    object->set_pass_id(b_ob.index);
    const float *object_color = b_ob.color;
    object->set_color(make_float3(object_color[0], object_color[1], object_color[2]));
    object->set_alpha(object_color[3]);
    object->set_tfm(tfm);

    /* dupli texture coordinates and random_id */
    if (is_instance) {
      const float *orco = b_deg_iter_data.dupli_object_current->orco;
      object->set_dupli_generated(0.5f * make_float3(orco[0], orco[1], orco[2]) -
                                  make_float3(0.5f, 0.5f, 0.5f));
      const float *uv = b_deg_iter_data.dupli_object_current->uv;
      object->set_dupli_uv(make_float2(uv[0], uv[1]));
      object->set_random_id(b_deg_iter_data.dupli_object_current->random_id);
    }
    else {
      object->set_dupli_generated(zero_float3());
      object->set_dupli_uv(zero_float2());
      object->set_random_id(hash_uint2(hash_string(object->name.c_str()), 0));
    }

    /* Light group and linking. */
    string lightgroup = b_ob.lightgroup ? b_ob.lightgroup->name : "";
    if (lightgroup.empty()) {
      lightgroup = b_parent->lightgroup ? b_parent->lightgroup->name : "";
    }
    object->set_lightgroup(ustring(lightgroup));

    object->set_light_set_membership(BlenderLightLink::get_light_set_membership(b_parent, b_ob));
    object->set_receiver_light_set(BlenderLightLink::get_receiver_light_set(b_parent, b_ob));
    object->set_shadow_set_membership(BlenderLightLink::get_shadow_set_membership(b_parent, b_ob));
    object->set_blocker_shadow_set(BlenderLightLink::get_blocker_shadow_set(b_parent, b_ob));
  }

  sync_object_motion_init(*b_parent, b_ob, object);

  if (do_sync || object->motion_is_modified()) {
    object->tag_update(scene);
  }

  if (is_instance) {
    /* Sync possible particle data. */
    sync_dupli_particle(*b_parent, b_deg_iter_data, b_ob, object);
  }

  return object;
}

static float4 lookup_instance_property(blender::Object &ob,
                                       blender::DEGObjectIterData &b_deg_iter_data,
                                       const string &name,
                                       bool use_instancer)
{
  blender::DupliObject *dupli = nullptr;
  blender::Object *dupli_parent = nullptr;

  /* If requesting instance data, check the parent particle system and object. */
  if (use_instancer && b_deg_iter_data.dupli_object_current) {
    dupli = b_deg_iter_data.dupli_object_current;
    dupli_parent = b_deg_iter_data.dupli_parent;
  }

  float4 value;
  BKE_object_dupli_find_rgba_attribute(&ob, dupli, dupli_parent, name.c_str(), &value.x);

  return value;
}

bool BlenderSync::sync_object_attributes(blender::Object &b_ob,
                                         blender::DEGObjectIterData &b_deg_iter_data,
                                         Object *object)
{
  /* Find which attributes are needed. */
  AttributeRequestSet requests = object->get_geometry()->needed_attributes();

  /* Delete attributes that became unnecessary. */
  vector<ParamValue> &attributes = object->attributes;
  bool changed = false;

  for (int i = attributes.size() - 1; i >= 0; i--) {
    if (!requests.find(attributes[i].name())) {
      attributes.erase(attributes.begin() + i);
      changed = true;
    }
  }

  /* Update attribute values. */
  for (const AttributeRequest &req : requests.requests) {
    const ustring name = req.name;

    std::string real_name;
    const int type = blender_attribute_name_split_type(name, &real_name);

    if (type == blender::SHD_ATTRIBUTE_OBJECT || type == blender::SHD_ATTRIBUTE_INSTANCER) {
      const bool use_instancer = (type == blender::SHD_ATTRIBUTE_INSTANCER);
      float4 value = lookup_instance_property(b_ob, b_deg_iter_data, real_name, use_instancer);

      /* Try finding the existing attribute value. */
      ParamValue *param = nullptr;

      for (size_t i = 0; i < attributes.size(); i++) {
        if (attributes[i].name() == name) {
          param = &attributes[i];
          break;
        }
      }

      /* Replace or add the value. */
      const ParamValue new_param(name, TypeFloat4, 1, &value);
      assert(new_param.datasize() == sizeof(value));

      if (!param) {
        changed = true;
        attributes.push_back(new_param);
      }
      else {
        /* Cannot use param->get<float4>, ParamValue storage is not guaranteed to be aligned. */
        const float *param_data = static_cast<const float *>(param->data());
        if (make_float4(param_data[0], param_data[1], param_data[2], param_data[3]) != value) {
          changed = true;
          *param = new_param;
        }
      }
    }
  }

  return changed;
}

/* Render Instances
 *
 * For a point cloud tagged with the "cycles_render_instancer" custom property,
 * read its evaluated points directly and create one Cycles object per point,
 * all sharing a single prototype geometry. This bypasses object_duplilist(),
 * which the depsgraph iterator otherwise re-runs in full on every sync.
 *
 * M0: position and uniform scale only. No orient, collections, materials or
 * motion blur yet. */

bool BlenderSync::object_is_render_instancer(blender::Object &b_ob)
{
  if (b_ob.id.properties == nullptr) {
    return false;
  }
  const blender::IDProperty *prop = blender::IDP_GetPropertyFromGroup(
      b_ob.id.properties, "cycles_render_instancer");
  if (prop == nullptr) {
    return false;
  }
  /* Python bools arrive as IDP_BOOLEAN, but older files may carry IDP_INT.
   * Both store the value in data.val (see IDP_bool_get / IDP_int_get). */
  if (prop->type != blender::IDP_BOOLEAN && prop->type != blender::IDP_INT) {
    return false;
  }
  if (prop->data.val == 0) {
    return false;
  }
  /* Only meaningful when geometry nodes actually produced unrealized
   * instances for us to read. */
  const blender::bke::GeometrySet *b_geometry_set = b_ob.runtime->geometry_set_eval;
  return b_geometry_set != nullptr && b_geometry_set->has_instances();
}

/* Flatten one instance reference into the geometry it actually draws, with
 * each piece's transform relative to the instance. Mirrors what
 * object_duplilist() would have produced:
 *   - Object      -> the object's own geometry
 *   - GeometrySet -> its mesh, and/or nested instances (recursed)
 *   - Collection  -> every visible member, offset by instance_offset
 *                    (see make_duplis_collection in object_dupli.cc)
 */
void BlenderSync::render_instances_collect_prototypes(
    const blender::bke::InstanceReference &b_reference,
    const Transform &local,
    blender::Object &b_instancer,
    vector<RenderInstanceProto> &r_protos,
    const int depth)
{
  /* Same recursion bound Blender uses for duplis. */
  if (depth >= 8) {
    LOG_INFO << "render-instances: recursion limit reached, nested instances truncated";
    return;
  }

  switch (b_reference.type()) {
    case blender::bke::InstanceReference::Type::Object: {
      blender::Object *b_proto = &b_reference.object();
      const bool adaptive = object_subdivision_type(*b_proto, preview, use_adaptive_subdivision) !=
                            Mesh::SUBDIVISION_NONE;
      BObjectInfo b_proto_info{b_proto, b_proto, object_get_data(*b_proto, adaptive), adaptive};
      if (!object_is_geometry(b_proto_info)) {
        return;
      }
      Geometry *geometry = sync_geometry(b_proto_info, false, false, nullptr);
      if (geometry) {
        r_protos.push_back({geometry,
                            local,
                            object_ray_visibility(*b_proto),
                            (b_proto->visibility_flag & blender::OB_SHADOW_CATCHER) != 0});
      }
      return;
    }

    case blender::bke::InstanceReference::Type::GeometrySet: {
      const blender::bke::GeometrySet &b_set = b_reference.geometry_set();

      const blender::Mesh *b_mesh = b_set.get_mesh();
      if (b_mesh != nullptr) {
        blender::ID *b_mesh_id = const_cast<blender::ID *>(&b_mesh->id);
        BObjectInfo b_proto_info{&b_instancer, &b_instancer, b_mesh_id, false};
        Geometry *geometry = sync_geometry(b_proto_info, false, false, nullptr);
        if (geometry) {
          /* sync_geometry resolves materials from the *object's* slots
           * (find_used_shaders, geometry.cpp:112). The instancer has none, so
           * anonymous geometry would silently render with the default shader.
           * Its materials live on the mesh itself, so resolve them from there.
           * Slot order matches, so the per-face shader indices stay valid. */
          array<Node *> mesh_shaders;
          blender::Material *material_override = view_layer.material_override;
          for (int m = 0; m < b_mesh->totcol; m++) {
            if (material_override) {
              find_shader(&material_override->id, mesh_shaders, scene->default_surface);
            }
            else {
              find_shader(reinterpret_cast<blender::ID *>(b_mesh->mat[m]),
                          mesh_shaders,
                          scene->default_surface);
            }
          }
          if (!mesh_shaders.empty()) {
            geometry->set_used_shaders(mesh_shaders);
          }

          /* Anonymous geometry owns no object, so it carries no flags of its
           * own; everything comes from the instancer. */
          r_protos.push_back({geometry, local, PathRayVisibility(PATH_RAY_VISIBILITY_ALL), false});
        }
      }

      /* Nested instances, e.g. Object Info with "As Instance" feeding
       * Instance on Points. */
      const blender::bke::Instances *b_nested = b_set.get_instances();
      if (b_nested != nullptr) {
        const blender::Span<blender::float4x4> b_nested_tfms = b_nested->transforms();
        const blender::Span<int> b_nested_handles = b_nested->reference_handles();
        const blender::Span<blender::bke::InstanceReference> b_nested_refs =
            b_nested->references();
        for (const int i : blender::IndexRange(b_nested_tfms.size())) {
          render_instances_collect_prototypes(b_nested_refs[b_nested_handles[i]],
                                              local * get_transform(b_nested_tfms[i]),
                                              b_instancer,
                                              r_protos,
                                              depth + 1);
        }
      }
      return;
    }

    case blender::bke::InstanceReference::Type::Collection: {
      blender::Collection &b_collection = b_reference.collection();

      /* The collection offset applies before member transforms. */
      const Transform collection_local =
          local * transform_translate(-make_float3(b_collection.instance_offset[0],
                                                   b_collection.instance_offset[1],
                                                   b_collection.instance_offset[2]));

      const int base_flag = preview ? blender::BASE_ENABLED_VIEWPORT : blender::BASE_ENABLED_RENDER;
      const int hide_flag = preview ? blender::OB_HIDE_VIEWPORT : blender::OB_HIDE_RENDER;

      /* Cache list is already flattened over nested collections. */
      for (blender::Base *b_base = static_cast<blender::Base *>(
               blender::BKE_collection_object_cache_get(&b_collection).first);
           b_base;
           b_base = b_base->next)
      {
        blender::Object *b_member = b_base->object;
        if (b_member == nullptr || b_member == &b_instancer) {
          continue;
        }
        if ((b_base->flag & base_flag) == 0 || (b_member->visibility_flag & hide_flag) != 0) {
          continue;
        }
        const bool adaptive = object_subdivision_type(
                                  *b_member, preview, use_adaptive_subdivision) !=
                              Mesh::SUBDIVISION_NONE;
        BObjectInfo b_member_info{
            b_member, b_member, object_get_data(*b_member, adaptive), adaptive};
        if (!object_is_geometry(b_member_info)) {
          continue;
        }
        Geometry *geometry = sync_geometry(b_member_info, false, false, nullptr);
        if (geometry) {
          r_protos.push_back({geometry,
                              collection_local * get_transform(b_member->object_to_world()),
                              object_ray_visibility(*b_member),
                              (b_member->visibility_flag & blender::OB_SHADOW_CATCHER) != 0});
        }
      }
      return;
    }

    case blender::bke::InstanceReference::Type::None:
    default:
      return;
  }
}

void BlenderSync::render_instances_pre_sync()
{
  for (auto &entry : render_instance_sets) {
    entry.second.used = false;
  }
}

void BlenderSync::render_instances_post_sync()
{
  /* These objects are deliberately not in object_map, so id_map's sweep never
   * sees them. Free the sets whose instancer is gone or no longer tagged. */
  set<Object *> to_delete;
  for (auto it = render_instance_sets.begin(); it != render_instance_sets.end();) {
    if (it->second.used) {
      ++it;
      continue;
    }
    for (Object *object : it->second.objects) {
      to_delete.insert(object);
    }
    it = render_instance_sets.erase(it);
  }
  if (!to_delete.empty()) {
    scene->delete_nodes(to_delete);
  }

  render_instances_recalc.clear();
}

void BlenderSync::sync_render_instances(blender::Depsgraph &b_depsgraph,
                                        blender::ViewLayer &b_view_layer,
                                        blender::Object &b_ob,
                                        const float motion_time)
{
  (void)b_depsgraph;

  /* Read the instances geometry nodes already built, in their unrealized form.
   * This is the same data object_duplilist() would expand into a DupliObject
   * list; we consume it directly and skip that expansion. */
  const blender::bke::GeometrySet *b_geometry_set = b_ob.runtime->geometry_set_eval;
  if (b_geometry_set == nullptr) {
    return;
  }
  const blender::bke::Instances *b_instances = b_geometry_set->get_instances();
  if (b_instances == nullptr) {
    return;
  }

  const blender::Span<blender::float4x4> b_transforms = b_instances->transforms();
  const blender::Span<int> b_handles = b_instances->reference_handles();
  const blender::Span<blender::bke::InstanceReference> b_references = b_instances->references();
  const int num_instances = b_transforms.size();
  if (num_instances == 0) {
    return;
  }

  /* Flatten each distinct reference once into the geometry it draws. A
   * reference may expand to several pieces (collection members, nested
   * instances), so instances index into a per-handle list. */
  const int num_references = b_references.size();
  vector<vector<RenderInstanceProto>> protos_by_handle(num_references);
  int num_empty_refs = 0;

  for (int h = 0; h < num_references; h++) {
    render_instances_collect_prototypes(
        b_references[h], transform_identity(), b_ob, protos_by_handle[h], 0);
    if (protos_by_handle[h].empty()) {
      num_empty_refs++;
    }
  }

  /* Total objects needed: one per (instance, prototype piece). */
  size_t num_objects_needed = 0;
  for (int i = 0; i < num_instances; i++) {
    num_objects_needed += protos_by_handle[b_handles[i]].size();
  }
  if (num_objects_needed == 0) {
    LOG_INFO << "render-instances: no drawable geometry from " << num_references
             << " references, nothing synced";
    return;
  }

  /* Instancer-source Attribute nodes. Blender resolves these via
   * BKE_object_dupli_find_rgba_attribute -> find_geonode_attribute_rgba
   * (object_dupli.cc:1942), which reads the named attribute off the instances
   * component at the instance index. We have that component directly, so read
   * it the same way instead of needing a DupliObject.
   *
   * Only gathered when a shader actually asks for one, so scenes that do not
   * use instancer attributes pay nothing. */
  struct InstancerAttribute {
    ustring name;
    blender::VArray<blender::ColorGeometry4f> data;
  };
  vector<InstancerAttribute> instancer_attributes;
  {
    set<ustring> seen;
    const blender::bke::AttributeAccessor b_inst_attributes = b_instances->attributes();
    for (const vector<RenderInstanceProto> &protos : protos_by_handle) {
      for (const RenderInstanceProto &proto : protos) {
        if (proto.geometry == nullptr) {
          continue;
        }
        AttributeRequestSet requests = proto.geometry->needed_attributes();
        for (const AttributeRequest &req : requests.requests) {
          const ustring name = req.name;
          if (seen.find(name) != seen.end()) {
            continue;
          }
          std::string real_name;
          if (blender_attribute_name_split_type(name, &real_name) !=
              blender::SHD_ATTRIBUTE_INSTANCER)
          {
            continue;
          }
          seen.insert(name);
          blender::VArray<blender::ColorGeometry4f> data =
              *b_inst_attributes.lookup<blender::ColorGeometry4f>(real_name,
                                                                  blender::bke::AttrDomain::Instance);
          if (data) {
            instancer_attributes.push_back({name, std::move(data)});
          }
        }
      }
    }
  }
  if (!instancer_attributes.empty()) {
    LOG_INFO << "render-instances: " << instancer_attributes.size()
             << " instancer attribute(s) requested by shaders";
  }

  /* Instance transforms are relative to the instancer. */
  const Transform instancer_tfm = get_transform(b_ob.object_to_world());

  /* Per-instance flags, mirroring what sync_object() derives for a dupli.
   * The instancer plays the role of the dupli parent. */
  const blender::Base *base = BKE_view_layer_base_find(&b_view_layer, &b_ob);
  const bool use_holdout = (base && (base->flag & blender::BASE_HOLDOUT) != 0) ||
                           ((b_ob.visibility_flag & blender::OB_HOLDOUT) != 0);
  const bool instancer_shadow_catcher = (b_ob.visibility_flag & blender::OB_SHADOW_CATCHER) != 0;
  const PathRayVisibility instancer_visibility = object_ray_visibility(b_ob);
  const bool use_indirect_only = !use_holdout && base &&
                                 ((base->flag & blender::BASE_INDIRECT_ONLY) != 0);

  blender::PointerRNA b_ob_rna_ptr = RNA_id_pointer_create(&b_ob.id);
  blender::PointerRNA cobject = RNA_pointer_get(&b_ob_rna_ptr, "cycles");
  const float ao_distance = get_float(cobject, "ao_distance");
  const bool is_caustics_caster = get_boolean(cobject, "is_caustics_caster");
  const bool is_caustics_receiver = get_boolean(cobject, "is_caustics_receiver");
  const float shadow_terminator_shading_offset = b_ob.shadow_terminator_shading_offset;
  const float shadow_terminator_geometry_offset = b_ob.shadow_terminator_geometry_offset;

  /* Flat per-instancer storage, indexed by instance index. Objects persist
   * across syncs here instead of in object_map, so steady-state re-sync is an
   * array index rather than an ordered-map lookup. */
  RenderInstanceSet &instance_set = render_instance_sets[&b_ob];
  instance_set.used = true;

  /* Motion pass: sync_objects() is re-run once per motion step. Rebuilding
   * here would overwrite the center-time transforms with this step's, leaving
   * the instances at the wrong place and recording no motion at all. Instead
   * write this step into the existing objects' motion arrays. */
  if (motion_time != 0.0f) {
    const map<void *, RenderInstanceSet>::iterator it = render_instance_sets.find(&b_ob);
    if (it == render_instance_sets.end() || it->second.objects.size() != num_objects_needed) {
      return;
    }
    size_t motion_slot = 0;
    for (int i = 0; i < num_instances; i++) {
      const Transform instance_tfm = instancer_tfm * get_transform(b_transforms[i]);
      for (const RenderInstanceProto &proto : protos_by_handle[b_handles[i]]) {
        Object *object = it->second.objects[motion_slot++];
        if (object->use_motion()) {
          const int time_index = object->motion_step(motion_time);
          if (time_index >= 0) {
            object->set_motion_tfm(instance_tfm * proto.local, time_index);
          }
        }
      }
    }
    return;
  }

  /* If the depsgraph did not touch this instancer, its instances are already
   * correct from the previous sync and the whole per-instance loop can be
   * skipped -- this is what stops editing one instancer from stalling every
   * other one. Marking the set used above keeps the objects alive, and the
   * prototypes were re-synced above so geometry_map will not free them.
   *
   * Only safe when the object count is unchanged; otherwise fall through and
   * rebuild. */
  if (!render_instances_recalc.contains(&b_ob) &&
      instance_set.objects.size() == num_objects_needed && num_objects_needed > 0)
  {
    LOG_INFO << "render-instances: " << num_instances << " instances unchanged, loop skipped";
    return;
  }

  /* Object count shrank: free the tail. */
  if (instance_set.objects.size() > num_objects_needed) {
    set<Object *> to_delete;
    for (size_t i = num_objects_needed; i < instance_set.objects.size(); i++) {
      to_delete.insert(instance_set.objects[i]);
    }
    scene->delete_nodes(to_delete);
    instance_set.objects.resize(num_objects_needed);
  }

  const double t_start = time_dt();
  double t_map = 0.0;
  size_t slot = 0;

  for (int i = 0; i < num_instances; i++) {
    const Transform instance_tfm = instancer_tfm * get_transform(b_transforms[i]);

    for (const RenderInstanceProto &proto : protos_by_handle[b_handles[i]]) {
      const double t0 = time_dt();
      if (slot >= instance_set.objects.size()) {
        instance_set.objects.push_back(scene->create_node<Object>());
      }
      Object *object = instance_set.objects[slot];
      t_map += time_dt() - t0;

      object->set_geometry(proto.geometry);
      object->set_tfm(instance_tfm * proto.local);

      /* Visibility is the intersection of prototype and instancer, exactly as
       * sync_object() ANDs a dupli with its parent. */
      PathRayVisibility visibility = proto.visibility & instancer_visibility;
      if (use_indirect_only) {
        visibility &= ~PATH_RAY_VISIBILITY_CAMERA;
      }
      object->set_visibility(visibility);
      object->set_use_holdout(use_holdout);
      object->set_is_shadow_catcher(proto.is_shadow_catcher || instancer_shadow_catcher);
      object->set_ao_distance(ao_distance);
      object->set_is_caustics_caster(is_caustics_caster);
      object->set_is_caustics_receiver(is_caustics_receiver);
      object->set_shadow_terminator_shading_offset(shadow_terminator_shading_offset);
      object->set_shadow_terminator_geometry_offset(shadow_terminator_geometry_offset);

      /* No DupliObject to inherit from, so synthesize these. */
      object->set_random_id(hash_uint2(i, 0));
      object->set_dupli_generated(zero_float3());
      object->set_dupli_uv(zero_float2());

      /* Per-instance shader attributes. */
      if (!instancer_attributes.empty()) {
        object->attributes.clear();
        for (const InstancerAttribute &attr : instancer_attributes) {
          const blender::ColorGeometry4f c = attr.data[i];
          const float4 value = make_float4(c.r, c.g, c.b, c.a);
          object->attributes.push_back(ParamValue(attr.name, TypeFloat4, 1, &value));
        }
      }

      /* Size the motion array now; motion passes fill it in later. */
      if (scene->need_motion() != Scene::MOTION_NONE) {
        sync_object_motion_init(b_ob, b_ob, object);
      }

      object->tag_update(scene);
      slot++;
    }
  }

  const double t_total = time_dt() - t_start;
  LOG_INFO << "render-instances: " << num_instances << " instances -> " << slot
           << " objects, " << num_references << " refs (" << num_empty_refs << " empty) in "
           << t_total * 1000.0 << " ms (object_map " << t_map * 1000.0 << " ms, "
           << (t_total > 0.0 ? 100.0 * t_map / t_total : 0.0) << "%; build "
           << (t_total - t_map) * 1000.0 << " ms)";
}

/* Object Loop */

void BlenderSync::sync_objects(blender::Depsgraph &b_depsgraph,
                               blender::bScreen *b_screen,
                               blender::View3D *b_v3d,
                               const float motion_time)
{
  /* Task pool for multithreaded geometry sync. */
  TaskPool geom_task_pool;

  /* layer data */
  const bool motion = motion_time != 0.0f;

  if (!motion) {
    /* prepare for sync */
    geometry_map.pre_sync();
    object_map.pre_sync();
    render_instances_pre_sync();
    procedural_map.pre_sync();
    particle_system_map.pre_sync();
    motion_times.clear();
  }
  else {
    geometry_motion_synced.clear();
  }

  if (!motion) {
    /* Object to geometry instance mapping is built for the reference time, as other
     * times just look up the corresponding geometry. */
    instance_geometries_by_object.clear();
  }

  /* initialize culling */
  BlenderObjectCulling culling(scene, *b_scene);

  /* object loop */
  bool cancel = false;
  const bool show_lights =
      BlenderViewportParameters(b_screen, b_v3d, use_developer_ui).use_scene_lights;

  blender::ViewLayer &b_view_layer = *DEG_get_evaluated_view_layer(&b_depsgraph);

  BKE_view_layer_synced_ensure(*b_data, b_scene, &b_view_layer);

  blender::DEGObjectIterSettings deg_iter_settings{};
  deg_iter_settings.depsgraph = &b_depsgraph;
  deg_iter_settings.flags = DEG_OBJECT_ITER_FOR_RENDER_ENGINE_FLAGS;
  blender::DEGObjectIterData deg_iter_data{};
  deg_iter_data.settings = &deg_iter_settings;
  deg_iter_data.graph = deg_iter_settings.depsgraph;
  deg_iter_data.flag = deg_iter_settings.flags;

  ITER_BEGIN (blender::DEG_iterator_objects_begin,
              blender::DEG_iterator_objects_next,
              blender::DEG_iterator_objects_end,
              &deg_iter_data,
              blender::Object *,
              b_ob)
  {
    /* Viewport visibility. */
    const bool show_in_viewport = !b_v3d || BKE_object_is_visible_in_viewport(b_v3d, b_ob);
    if (show_in_viewport == false) {
      continue;
    }

    /* Render-instance source: build instances from the point data directly and
     * skip the normal path, so it is not also synced as a plain point cloud. */
    if (object_is_render_instancer(*b_ob)) {
      sync_render_instances(b_depsgraph, b_view_layer, *b_ob, motion_time);
      continue;
    }

    /* Load per-object culling data. */
    culling.init_object(scene, *b_ob);

    const int ob_visibility = BKE_object_visibility(b_ob, deg_iter_data.eval_mode);

    /* Ensure the object geom supporting the hair is processed before adding
     * the hair processing task to the task pool, calling .to_mesh() on the
     * same object in parallel does not work. */
    const bool sync_hair = (ob_visibility & blender::OB_VISIBLE_PARTICLES) != 0 &&
                           object_has_particle_hair(b_ob);

    /* Object itself. */
    if ((ob_visibility & blender::OB_VISIBLE_SELF) != 0) {
      sync_object(b_view_layer,
                  *b_ob,
                  deg_iter_data,
                  motion_time,
                  false,
                  show_lights,
                  culling,
                  sync_hair ? nullptr : &geom_task_pool);
    }

    /* Particle hair as separate object. */
    if (sync_hair) {
      sync_object(b_view_layer,
                  *b_ob,
                  deg_iter_data,
                  motion_time,
                  true,
                  show_lights,
                  culling,
                  &geom_task_pool);
    }

    cancel = progress.get_cancel();
    if (cancel) {
      break;
    }
  }
  ITER_END;

  geom_task_pool.wait_work();

  LOG_INFO << "render-instances: scene has " << scene->objects.size()
           << " cycles objects after sync";

  progress.set_sync_status("");

  if (!cancel && !motion) {
    /* After object for world_use_portal. */
    sync_background_light(b_screen, b_v3d);

    /* Handle removed data and modified pointers, as this may free memory, delete Nodes in the
     * right order to ensure that dependent data is freed after their users. Objects should be
     * freed before particle systems and geometries. */
    render_instances_post_sync();
    object_map.post_sync();
    geometry_map.post_sync();
    particle_system_map.post_sync();
    procedural_map.post_sync();
  }

  if (motion) {
    geometry_motion_synced.clear();
  }
}

void BlenderSync::sync_objects_and_motion(blender::RenderData &b_render,
                                          blender::Depsgraph &b_depsgraph,
                                          blender::bScreen *b_screen,
                                          blender::View3D *b_v3d,
                                          blender::RegionView3D *b_rv3d,
                                          const int width,
                                          const int height,
                                          void **python_thread_state)
{
  /* get camera object here to deal with camera switch */
  blender::Object *b_cam = get_camera_object(b_v3d, b_rv3d);

  const int frame_center = b_scene->r.cfra;
  const float subframe_center = b_scene->r.subframe;
  float frame_center_delta = 0.0f;

  if (scene->need_motion() == Scene::MOTION_BLUR &&
      scene->camera->get_motion_position() != MOTION_POSITION_CENTER)
  {
    const float shuttertime = scene->camera->get_shuttertime();
    if (scene->camera->get_motion_position() == MOTION_POSITION_END) {
      frame_center_delta = -shuttertime * 0.5f;
    }
    else {
      assert(scene->camera->get_motion_position() == MOTION_POSITION_START);
      frame_center_delta = shuttertime * 0.5f;
    }

    const float time = frame_center + subframe_center + frame_center_delta;
    const int frame = (int)floorf(time);
    const float subframe = time - frame;
    python_thread_state_restore(python_thread_state);
    RE_engine_frame_set(b_engine, frame, subframe);
    python_thread_state_save(python_thread_state);
    if (b_cam) {
      sync_camera_motion(b_render, b_cam, width, height, 0.0f);
    }
  }

  sync_objects(b_depsgraph, b_screen, b_v3d);

  /* In the viewport, only motion between previous frame and current frame is of interest, which is
   * kept updated separately. */
  if (b_v3d) {
    assert(scene->need_motion() == Scene::MOTION_NONE ||
           scene->need_motion() == Scene::MOTION_PASS_INTERACTIVE);
    return;
  }

  if (scene->need_motion() == Scene::MOTION_NONE) {
    return;
  }

  /* Insert motion times from camera. Motion times from other objects
   * have already been added in a sync_objects call. */
  if (b_cam) {
    const uint camera_motion_steps = object_motion_steps(*b_cam, *b_cam);
    for (size_t step = 0; step < camera_motion_steps; step++) {
      motion_times.insert(scene->camera->motion_time(step));
    }
  }

  /* Check which geometry already has motion blur so it can be skipped. */
  geometry_motion_attribute_synced.clear();
  for (Geometry *geom : scene->geometry) {
    const Attribute *attr_P = geom->attributes.find(ATTR_STD_POSITION);
    if (attr_P && attr_P->has_motion()) {
      geometry_motion_attribute_synced.insert(geom);
    }
  }

  /* note iteration over motion_times set happens in sorted order */
  for (const float relative_time : motion_times) {
    /* center time is already handled. */
    if (relative_time == 0.0f) {
      continue;
    }

    LOG_DEBUG << "Synchronizing motion for the relative time " << relative_time << ".";

    /* fixed shutter time to get previous and next frame for motion pass */
    const float shuttertime = scene->motion_shutter_time();

    /* compute frame and subframe time */
    const float time = frame_center + subframe_center + frame_center_delta +
                       relative_time * shuttertime * 0.5f;
    const int frame = (int)floorf(time);
    const float subframe = time - frame;

    /* change frame */
    python_thread_state_restore(python_thread_state);
    RE_engine_frame_set(b_engine, frame, subframe);
    python_thread_state_save(python_thread_state);

    /* Syncs camera motion if relative_time is one of the camera's motion times. */
    sync_camera_motion(b_render, b_cam, width, height, relative_time);

    /* sync object */
    sync_objects(b_depsgraph, b_screen, b_v3d, relative_time);
  }

  geometry_motion_attribute_synced.clear();

  /* we need to set the python thread state again because this
   * function assumes it is being executed from python and will
   * try to save the thread state */
  python_thread_state_restore(python_thread_state);
  RE_engine_frame_set(b_engine, frame_center, subframe_center);
  python_thread_state_save(python_thread_state);
}

CCL_NAMESPACE_END
