/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_camera_types.h"

#include "BKE_camera.h"
#include "BKE_scene.hh"

#include "DEG_depsgraph_query.hh"

#include "COM_node_operation.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_camera_panoramic_info_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.add_output<decl::Float>("Field of View"_ustr).description("Panoramic camera field of view");
  b.add_output<decl::Float>("Fisheye Lens"_ustr).description("Panoramic camera Lens");
  b.add_output<decl::Vector>("Sensor"_ustr).dimensions(2).description("Size of the camera sensor");
  b.add_output<decl::Vector>("Shift"_ustr).dimensions(2).description("Camera shift");
  b.add_output<decl::Float>("Clip Start"_ustr).description("Camera near clipping distance");
  b.add_output<decl::Float>("Clip End"_ustr).description("Camera far clipping distance");
  b.add_output<decl::Float>("Focus Distance"_ustr)
      .description("Distance to the focus point for depth of field");

  
  {
    auto &p = b.add_panel("Types"_ustr).default_closed(true);
    p.add_output<decl::Bool>("Is Equirectangular"_ustr);     
    p.add_output<decl::Bool>("Is Equiangular"_ustr);
    p.add_output<decl::Bool>("Is Mirrorball"_ustr);
    p.add_output<decl::Bool>("Is Equisolid"_ustr);
    p.add_output<decl::Bool>("Is Equidistant"_ustr);
    p.add_output<decl::Bool>("Is Polynomal"_ustr);
    p.add_output<decl::Bool>("Is Cylindrical"_ustr);

  }

  {
      auto &p = b.add_panel("Equirectangular"_ustr).default_closed(true);
      p.add_output<decl::Float>("Latitude Min"_ustr)
    .description("Latitude Min");

      p.add_output<decl::Float>("Latitude Max"_ustr)
    .description("Latitude Max");

      p.add_output<decl::Float>("Longitude Min"_ustr)
    .description("Longitude Max");
      p.add_output<decl::Float>("Longitude Max"_ustr)
    .description("Longitude Max");
  }
  
   {
       auto &p = b.add_panel("Central Cylindrical"_ustr).default_closed(true);
       p.add_output<decl::Float>("Height Min"_ustr)
      .description("Height Min");
       p.add_output<decl::Float>("Height Max"_ustr)
      .description("Height Min");
       p.add_output<decl::Float>("Longitude Min(Cylindrical)"_ustr)
      .description("Longitude Max");
       p.add_output<decl::Float>("Longitude Max(Cylindrical)"_ustr)
      .description("Longitude Max");
       p.add_output<decl::Float>("Cylinder Radius"_ustr)
      .description("Longitude Max");
  }

    {
       auto &p = b.add_panel("Polynomal"_ustr).default_closed(true);
       p.add_output<decl::Float>("K0"_ustr)
       .description("K0");
       p.add_output<decl::Float>("K1"_ustr)
       .description("K1");
       p.add_output<decl::Float>("K2"_ustr)
       .description("K2");
       p.add_output<decl::Float>("K3"_ustr)
       .description("K3");
       p.add_output<decl::Float>("K4"_ustr)
       .description("K4");
    } 

  b.add_input<decl::Object>("Camera"_ustr).optional_label();
}

static CameraParams get_camera_parameters(const Scene &scene, const Object &camera_object)
{
  CameraParams camera_params;
  BKE_camera_params_init(&camera_params);
  BKE_camera_params_from_object(&camera_params, &camera_object);
  BKE_camera_params_compute_viewplane(
      &camera_params, scene.r.xsch, scene.r.ysch, scene.r.xasp, scene.r.yasp);
  /* BKE_camera_params_compute_matrix(&camera_params); */
  return camera_params;
}

/* Computes the horizontal aspect ratio and effective sensor fit of the camera. */
static void compute_aspect_ratio_and_effective_sensor_fit(const Scene &scene,
                                                          const CameraParams &camera_parameters,
                                                          float &aspect_ratio,
                                                          eCamera_SensorFit &effective_sensor_fit)
{
  int2 resolution;
  BKE_render_resolution(&scene.r, false, &resolution.x, &resolution.y);
  const float2 aspect_corrected_resolution = float2(resolution) *
                                             float2(scene.r.xasp, scene.r.yasp);

  aspect_ratio = aspect_corrected_resolution.x / aspect_corrected_resolution.y;
  effective_sensor_fit = eCamera_SensorFit(BKE_camera_sensor_fit(
      camera_parameters.sensor_fit, aspect_corrected_resolution.x, aspect_corrected_resolution.y));
}

static float2 compute_sensor_size(const Scene &scene, const CameraParams &camera_parameters)
{
  float aspect_ratio;
  eCamera_SensorFit effective_sensor_fit;
  compute_aspect_ratio_and_effective_sensor_fit(
      scene, camera_parameters, aspect_ratio, effective_sensor_fit);

  const float sensor_size = BKE_camera_sensor_size(
      camera_parameters.sensor_fit, camera_parameters.sensor_x, camera_parameters.sensor_y);

  if (effective_sensor_fit == CAMERA_SENSOR_FIT_HOR) {
    return float2(sensor_size, sensor_size / aspect_ratio);
  }
  return float2(sensor_size * aspect_ratio, sensor_size);
}

static float2 compute_lens_shift(const Scene &scene, const CameraParams &camera_parameters)
{
  float aspect_ratio;
  eCamera_SensorFit effective_sensor_fit;
  compute_aspect_ratio_and_effective_sensor_fit(
      scene, camera_parameters, aspect_ratio, effective_sensor_fit);

  if (effective_sensor_fit == CAMERA_SENSOR_FIT_HOR) {
    return float2(camera_parameters.shiftx, camera_parameters.shifty * aspect_ratio);
  }
  return float2(camera_parameters.shiftx / aspect_ratio, camera_parameters.shifty);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const Scene *scene = DEG_get_evaluated_scene(params.depsgraph());
  if (!scene) {
    params.set_default_remaining_outputs();
    return;
  }

  const Object *camera_obj = params.extract_input<Object *>("Camera"_ustr);

  if (!camera_obj || camera_obj->type != OB_CAMERA) {
    params.set_default_remaining_outputs();
    return;
  }

  Camera *camera = id_cast<Camera *>(camera_obj->data);
  if (!camera) {
    params.set_default_remaining_outputs();
    return;
  }

  const CameraParams camera_params = get_camera_parameters(*scene, *camera_obj);
  const float focus_distance = BKE_camera_object_dof_distance(camera_obj);
  const float2 sensor_size = compute_sensor_size(*scene, camera_params);
  const float2 lens_shift = compute_lens_shift(*scene, camera_params);

  /* Mostly Used*/
  params.set_output("Field of View"_ustr, camera_params.fisheye_fov); 
  params.set_output("Fisheye Lens"_ustr, camera_params.fisheye_lens); 
  params.set_output("Sensor"_ustr, float3{sensor_size, 0.0f});
  params.set_output("Shift"_ustr, float3{lens_shift, 0.0f});
  params.set_output("Clip Start"_ustr, camera_params.clip_start);
  params.set_output("Clip End"_ustr, camera_params.clip_end);
  params.set_output("Focus Distance"_ustr, focus_distance);

  /* Panoramic Types */
  params.set_output("Is Equirectangular"_ustr, camera_params.is_equirectangular);
  params.set_output("Is Equiangular"_ustr, camera_params.is_equiangular);
  params.set_output("Is Mirrorball"_ustr, camera_params.is_mirrorball);
  params.set_output("Is Equidistant"_ustr, camera_params.is_equidistant);
  params.set_output("Is Equisolid"_ustr, camera_params.is_equisolid);
  params.set_output("Is Polynomal"_ustr, camera_params.is_polynomal);
  params.set_output("Is Cylindrical"_ustr, camera_params.is_cylindrical); 
  
  /* Polynonmal */
  params.set_output("K0"_ustr, camera_params.k0);
  params.set_output("K1"_ustr, camera_params.k1);
  params.set_output("K2"_ustr, camera_params.k2);
  params.set_output("K3"_ustr, camera_params.k3);
  params.set_output("K4"_ustr, camera_params.k4);
    
  /* Equirectangular */
  params.set_output("Latitude Min"_ustr, camera_params.latitude_min);
  params.set_output("Latitude Max"_ustr, camera_params.latitude_max);
  params.set_output("Longitude Min"_ustr, camera_params.longitude_min);
  params.set_output("Longitude Max"_ustr, camera_params.longitude_max); 

  /* Central Cylindrical */
  params.set_output("Height Min"_ustr, camera_params.cylindrical_height_min);
  params.set_output("Height Max"_ustr, camera_params.cylindrical_height_max);
  params.set_output("Longitude Min(Cylindrical)"_ustr, camera_params.cylindrical_longitude_min);
  params.set_output("Longitude Max(Cylindrical)"_ustr, camera_params.cylindrical_longitude_max);

}

using namespace blender::compositor;

class CameraPanoramicInfoOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Object *camera_object = this->get_input("Camera").get_single_value<Object *>();
    if (!camera_object || camera_object->type != OB_CAMERA) {
      this->allocate_default_remaining_outputs();
      return;
    }

    const Camera *camera = id_cast<Camera *>(camera_object->data);
    if (!camera) {
      this->allocate_default_remaining_outputs();
      return;
    }

    const Scene &scene = this->context().get_scene();
    const CameraParams camera_parameters = get_camera_parameters(scene, *camera_object);

    const float2 sensor_size = compute_sensor_size(scene, camera_parameters);
    Result &sensor_result = this->get_result("Sensor");
    if (sensor_result.should_compute()) {
      sensor_result.allocate_single_value();
      sensor_result.set_single_value(sensor_size);
    }

    const float2 lens_shift = compute_lens_shift(scene, camera_parameters);
    Result &shift_result = this->get_result("Shift");
    if (shift_result.should_compute()) {
      shift_result.allocate_single_value();
      shift_result.set_single_value(lens_shift);
    }

    Result &clip_start_result = this->get_result("Clip Start");
    if (clip_start_result.should_compute()) {
      clip_start_result.allocate_single_value();
      clip_start_result.set_single_value(camera_parameters.clip_start);
    }

    Result &clip_end_result = this->get_result("Clip End");
    if (clip_end_result.should_compute()) {
      clip_end_result.allocate_single_value();
      clip_end_result.set_single_value(camera_parameters.clip_end);
    }

    Result &focus_distance_result = this->get_result("Focus Distance");
    if (focus_distance_result.should_compute()) {
      focus_distance_result.allocate_single_value();
      const float focus_distance = BKE_camera_object_dof_distance(camera_object);
      focus_distance_result.set_single_value(focus_distance);
    }

  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new CameraPanoramicInfoOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_cmp_node_type_base(&ntype, "GeometryNodeCameraPanoramicInfo"_ustr);
  ntype.ui_name = "Camera Panoramic Info";
  ntype.ui_description = "Retrieve information from a panoramic camera object";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_camera_info_cc
