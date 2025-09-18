/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "scene/light.h"

#include "DNA_light_types.h"

#include "IMB_colormanagement.hh"

#include "blender/image.h"
#include "blender/sync.h"
#include "blender/util.h"
#include "scene/colorspace.h"
#include "scene/object.h"

CCL_NAMESPACE_BEGIN

void BlenderSync::sync_light(BObjectInfo &b_ob_info, Light *light)
{
  BL::Light b_light(b_ob_info.object_data);

  light->name = b_light.name().c_str();

  /* type */
  switch (b_light.type()) {
    case BL::Light::type_POINT: {
      BL::PointLight b_point_light(b_light);
      light->set_size(b_point_light.shadow_soft_size());
      light->set_light_type(LIGHT_POINT);
      light->set_is_sphere(!b_point_light.use_soft_falloff());
      break;
    }
    case BL::Light::type_SPOT: {
      BL::SpotLight b_spot_light(b_light);
      light->set_size(b_spot_light.shadow_soft_size());
      light->set_light_type(LIGHT_SPOT);
      light->set_spot_angle(b_spot_light.spot_size());
      light->set_spot_smooth(b_spot_light.spot_blend());
      light->set_is_sphere(!b_spot_light.use_soft_falloff());
      break;
    }
    /* Hemi were removed from 2.8 */
    // case BL::Light::type_HEMI: {
    //  light->type = LIGHT_DISTANT;
    //  light->size = 0.0f;
    //  break;
    // }
    case BL::Light::type_SUN: {
      BL::SunLight b_sun_light(b_light);
      light->set_angle(b_sun_light.angle());
      light->set_light_type(LIGHT_DISTANT);
      break;
    }
    case BL::Light::type_AREA: {
      BL::AreaLight b_area_light(b_light);
      light->set_size(1.0f);
      light->set_sizeu(b_area_light.size());
      light->set_spread(b_area_light.spread());
      switch (b_area_light.shape()) {
        case BL::AreaLight::shape_SQUARE:
          light->set_sizev(light->get_sizeu());
          light->set_ellipse(false);
          break;
        case BL::AreaLight::shape_RECTANGLE:
          light->set_sizev(b_area_light.size_y());
          light->set_ellipse(false);
          break;
        case BL::AreaLight::shape_DISK:
          light->set_sizev(light->get_sizeu());
          light->set_ellipse(true);
          break;
        case BL::AreaLight::shape_ELLIPSE:
          light->set_sizev(b_area_light.size_y());
          light->set_ellipse(true);
          break;
      }
      light->set_light_type(LIGHT_AREA);
      break;
    }
    case BL::Light::type_DOME: {
      BL::DomeLight b_dome_light(b_light);
      light->set_size(b_dome_light.dome_size());
      light->set_light_type(LIGHT_DOME);

      /* Set dome type (spherical or hemisphere) */
      light->set_is_dome_hemisphere(b_dome_light.dome_type() == 1); /* LA_DOME_HEMISPHERE = 1 */

      /* Set up HDRI image if available - focus on safe image handling during live updates */
      BL::Image b_image = b_dome_light.dome_image();
      if (b_image) {
        /* Get image filepath like other lights do */
        string image_filename = b_image.filepath();
        if (image_filename.empty() && b_image.packed_file()) {
          /* Handle packed images by using a unique identifier */
          image_filename = b_image.name();
        }

        if (!image_filename.empty()) {
          light->set_dome_image(ustring(image_filename));
          light->set_dome_hdr_strength(b_dome_light.dome_hdr_strength());

          /* The key fix: Only reload HDR texture if the image actually changed
           * This prevents crashes during live property updates */
          ustring current_image = light->get_dome_image();
          if (current_image != ustring(image_filename) || light->dome_hdr_handle.empty()) {

            /* Clear old HDR texture handle before loading new one to prevent crashes */
            light->clear_dome_hdr_texture();

            /* Load HDR texture using BlenderImageLoader */
            ImageParams params;
            params.interpolation = INTERPOLATION_LINEAR;
            params.extension = EXTENSION_REPEAT;
            params.alpha_type = IMAGE_ALPHA_AUTO;
            params.colorspace =
                u_colorspace_raw; /* HDR images are already in linear space, no transform needed */

            /* Create a default ImageUser for now */
            ImageUser image_user = {nullptr};

            light->dome_hdr_handle = scene->image_manager->add_image(
                make_unique<BlenderImageLoader>(static_cast<::Image *>(b_image.ptr.data),
                                                &image_user,
                                                0, /* frame */
                                                0, /* tile */
                                                b_engine.is_preview()),
                params);
          }
          else {
          }

          /* Enable MIS for dome light with image */
          light->set_use_mis(true);

          /* Use the user-specified map resolution for image sampling */
          light->set_map_resolution(b_dome_light.dome_map_resolution());
        }
      }
      else {
        /* Clear dome image and intensity */
        light->set_dome_image(ustring());
        light->set_dome_hdr_strength(1.0f);
        light->clear_dome_hdr_texture();

        /* Dome light without image still needs MIS enabled */
        light->set_use_mis(true);
      }

      /* Set dome HDR rotation (combined with object rotation) */
      float3 dome_rotation = get_float3(b_dome_light.dome_rotation());
      light->set_dome_rotation(dome_rotation);

      /* Set dome HDR gamma correction */
      float dome_gamma = b_dome_light.dome_hdr_gamma();
      light->set_dome_hdr_gamma(dome_gamma);

      /* Set dome HDR U/V flipping options */
      bool dome_flip_u = b_dome_light.dome_hdr_flip_u();
      bool dome_flip_v = b_dome_light.dome_hdr_flip_v();
      light->set_dome_hdr_flip_u(dome_flip_u);
      light->set_dome_hdr_flip_v(dome_flip_v);

      /* Set a default average radiance for light tree compatibility */
      light->set_average_radiance(1.0f);
      break;
    }
  }

  /* Color and strength. */
  float3 light_color = get_float3(b_light.color());
  if (b_light.use_temperature()) {
    light_color *= get_float3(b_light.temperature_color());
  }

  const float3 strength = light_color * BL::PointLight(b_light).energy() *
                          exp2f(b_light.exposure());
  light->set_strength(strength);

  /* normalize */
  light->set_normalize(b_light.normalize());

  /* shadow */
  PointerRNA clight = RNA_pointer_get(&b_light.ptr, "cycles");
  light->set_cast_shadow(b_light.use_shadow());
  light->set_use_mis(get_boolean(clight, "use_multiple_importance_sampling"));

  /* caustics light */
  light->set_use_caustics(get_boolean(clight, "is_caustics_light"));

  light->set_max_bounces(get_int(clight, "max_bounces"));

  if (light->get_light_type() == LIGHT_AREA) {
    light->set_is_portal(get_boolean(clight, "is_portal"));
  }
  else {
    light->set_is_portal(false);
  }

  if (light->get_is_portal()) {
    world_use_portal = true;
  }

  /* tag */
  light->tag_update(scene);
}

void BlenderSync::sync_background_light(BL::SpaceView3D &b_v3d)
{
  BL::World b_world = view_layer.world_override ? view_layer.world_override : b_scene.world();

  if (b_world) {
    PointerRNA cworld = RNA_pointer_get(&b_world.ptr, "cycles");

    enum SamplingMethod { SAMPLING_NONE = 0, SAMPLING_AUTOMATIC, SAMPLING_MANUAL, SAMPLING_NUM };
    const int sampling_method = get_enum(
        cworld, "sampling_method", SAMPLING_NUM, SAMPLING_AUTOMATIC);
    const bool sample_as_light = (sampling_method != SAMPLING_NONE);

    /* Create object. */
    Object *object;
    const ObjectKey object_key(b_world, nullptr, b_world, false);
    bool update = object_map.add_or_update(&object, b_world, b_world, object_key);
    if (update) {
      /* Lights should be shadow catchers by default. */
      object->set_is_shadow_catcher(true);
      object->set_lightgroup(ustring(b_world ? b_world.lightgroup() : ""));
    }

    object->set_asset_name(ustring(b_world.name()));

    /* Create geometry. */
    const GeometryKey geom_key{b_world.ptr.data, Geometry::LIGHT};
    Geometry *geom = geometry_map.find(geom_key);
    if (geom) {
      update |= geometry_map.update(geom, b_world);
    }
    else {
      geom = scene->create_node<Light>();
      geometry_map.add(geom_key, geom);
      object->set_geometry(geom);
      update = true;
    }

    if (update || world_recalc || b_world.ptr.data != world_map) {
      /* Initialize light geometry. */
      Light *light = static_cast<Light *>(geom);

      array<Node *> used_shaders;
      used_shaders.push_back_slow(scene->default_background);
      light->set_used_shaders(used_shaders);

      light->set_light_type(LIGHT_BACKGROUND);

      if (sampling_method == SAMPLING_MANUAL) {
        light->set_map_resolution(get_int(cworld, "sample_map_resolution"));
      }
      else {
        light->set_map_resolution(0);
      }

      light->set_use_mis(sample_as_light);
      light->set_max_bounces(get_int(cworld, "max_bounces"));

      /* Force enable light again when world is resynced. */
      light->set_is_enabled(sample_as_light || world_use_portal);

      /* Caustic light. */
      light->set_use_caustics(get_boolean(cworld, "is_caustics_light"));

      light->tag_update(scene);

      geometry_map.set_recalc(b_world);
    }
  }

  world_map = b_world.ptr.data;
  world_recalc = false;
  viewport_parameters = BlenderViewportParameters(b_v3d, use_developer_ui);
}

CCL_NAMESPACE_END
