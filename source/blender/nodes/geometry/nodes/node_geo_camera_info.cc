#include "BKE_camera.h"
#include "BLI_math_matrix.hh"
#include "DEG_depsgraph_query.hh"
#include "DNA_camera_types.h"
#include "node_geometry_util.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_geo_camera_info_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();

  b.add_output<decl::Matrix>("Projection Matrix");
  b.add_output<decl::Bool>("Is Active Camera");
  b.add_input<decl::Object>("Camera").hide_label();

  PanelDeclarationBuilder &foc = b.add_panel("Focal").default_closed(true);
  foc.add_output<decl::Float>("Focal Length");
  foc.add_output<decl::Vector>("Sensor");
  foc.add_output<decl::Vector>("Shift");
  foc.add_output<decl::Float>("Clip Start");
  foc.add_output<decl::Float>("Clip End");
  foc.add_output<decl::Int>("Resolution X");
  foc.add_output<decl::Int>("Resolution Y");
  foc.add_output<decl::Vector>("Aspect");

  PanelDeclarationBuilder &dof = b.add_panel("Depth of Field").default_closed(true);
  dof.add_output<decl::Bool>("Depth of Field Enabled");
  dof.add_output<decl::Float>("Focus Distance");
  dof.add_output<decl::Float>("Aperture F-Stop");
  dof.add_output<decl::Int>("Aperture Blades");
  dof.add_output<decl::Float>("Aperture Rotation");
  dof.add_output<decl::Float>("Aperture Ratio");

  PanelDeclarationBuilder &ortho = b.add_panel("Orthographic").default_closed(true);
  ortho.add_output<decl::Bool>("Is Orthographic");
  ortho.add_output<decl::Float>("Orthographic Scale");

  PanelDeclarationBuilder &pano = b.add_panel("Panoramic").default_closed(true);
  pano.add_output<decl::Bool>("Is Panoramic");
  pano.add_output<decl::Float>("Fisheye FOV");
  pano.add_output<decl::Float>("Fisheye Lens");
  pano.add_output<decl::Float>("Latitude Min");
  pano.add_output<decl::Float>("Latitude Max");
  pano.add_output<decl::Float>("Longitude Min");
  pano.add_output<decl::Float>("Longitude Max");
  pano.add_output<decl::Float>("Fisheye Polynomial K0");
  pano.add_output<decl::Float>("Fisheye Polynomial K1");
  pano.add_output<decl::Float>("Fisheye Polynomial K2");
  pano.add_output<decl::Float>("Fisheye Polynomial K3");
  pano.add_output<decl::Float>("Fisheye Polynomial K4");

  PanelDeclarationBuilder &ster = b.add_panel("Stereo").default_closed(true);
  ster.add_output<decl::Float>("Interocular Distance");
  ster.add_output<decl::Float>("Convergence Distance");
  ster.add_output<decl::Float>("Pole Merge Angle From");
  ster.add_output<decl::Float>("Pole Merge Angle To");
}

static void node_geo_exec(GeoNodeExecParams params)
{

  const Scene *scene = DEG_get_evaluated_scene(params.depsgraph());
  if (!scene) {
    params.set_default_remaining_outputs();
    return;
  }

  Object *camera_obj = params.get_input<Object *>("Camera");

  if (!camera_obj || camera_obj->type != OB_CAMERA) {
    params.set_default_remaining_outputs();
    return;
  }

  Camera *camera = (Camera *)camera_obj->data;
  if (!camera) {
    params.set_default_remaining_outputs();
    return;
  }

  CameraParams camera_params;
  BKE_camera_params_init(&camera_params);
  BKE_camera_params_from_object(&camera_params, camera_obj);
  BKE_camera_params_compute_viewplane(
      &camera_params, scene->r.xsch, scene->r.ysch, scene->r.xasp, scene->r.yasp);
  BKE_camera_params_compute_matrix(&camera_params);

  float4x4 projection_matrix(camera_params.winmat);

  params.set_output("Projection Matrix", projection_matrix);
  params.set_output("Is Active Camera", scene->camera == camera_obj);

  params.set_output("Focal Length", camera_params.lens);
  params.set_output("Sensor", float3{camera_params.sensor_x, camera_params.sensor_y, 0.0f});
  params.set_output("Shift", float3{camera_params.shiftx, camera_params.shifty, 0.0f});
  params.set_output("Clip Start", camera_params.clip_start);
  params.set_output("Clip End", camera_params.clip_end);
  params.set_output("Resolution X", scene->r.xsch);
  params.set_output("Resolution Y", scene->r.ysch);
  params.set_output("Aspect", float3{scene->r.xasp, scene->r.yasp, 0.0f});

  params.set_output("Depth of Field Enabled", camera->dof.flag == CAM_DOF_ENABLED);
  params.set_output("Focus Distance", camera->dof.focus_distance);
  params.set_output("Aperture F-Stop", camera->dof.aperture_fstop);
  params.set_output("Aperture Blades", camera->dof.aperture_blades);
  params.set_output("Aperture Rotation", camera->dof.aperture_rotation);
  params.set_output("Aperture Ratio", camera->dof.aperture_ratio);

  params.set_output("Is Orthographic", camera_params.is_ortho);
  params.set_output("Orthographic Scale", camera_params.ortho_scale);

  params.set_output("Is Panoramic", camera->type == CAM_PANO);
  params.set_output("Fisheye FOV", camera->fisheye_fov);
  params.set_output("Fisheye Lens", camera->fisheye_lens);
  params.set_output("Latitude Min", camera->latitude_min);
  params.set_output("Latitude Max", camera->latitude_max);
  params.set_output("Longitude Min", camera->longitude_min);
  params.set_output("Longitude Max", camera->longitude_max);
  params.set_output("Fisheye Polynomial K0", camera->fisheye_polynomial_k0);
  params.set_output("Fisheye Polynomial K1", camera->fisheye_polynomial_k1);
  params.set_output("Fisheye Polynomial K2", camera->fisheye_polynomial_k2);
  params.set_output("Fisheye Polynomial K3", camera->fisheye_polynomial_k3);
  params.set_output("Fisheye Polynomial K4", camera->fisheye_polynomial_k4);

  params.set_output("Interocular Distance", camera->stereo.interocular_distance);
  params.set_output("Convergence Distance", camera->stereo.convergence_distance);
  params.set_output("Pole Merge Angle From", camera->stereo.pole_merge_angle_from);
  params.set_output("Pole Merge Angle To", camera->stereo.pole_merge_angle_to);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeCameraInfo");
  ntype.ui_name = "Camera Info";
  ntype.ui_description = "Retrieve information from a camera object";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_camera_info_cc
