#version 450
#extension GL_ARB_shader_draw_parameters : enable






#extension GL_ARB_shader_viewport_layer_array: enable
#extension GL_ARB_shader_stencil_export: enable



 





#pragma no_processing















                                                          
                                                          
                                                          
                                                          
                                                          
                                                          


                                                          






























































































































































struct string_t {
  uint hash;
};











 





 







































const bool use_cavity=false;
const bool use_curvature=false;
const bool use_shadow=false;
const int lighting_mode=0;















void main_function_();
void main() {
  main_function_();
gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
}



 





























 



















































 




struct                 DrawGroup {

  uint next;

  uint start;

  uint len;

  uint front_facing_len;

  int vertex_len;
  int vertex_first;

  int base_index;

  uint total_counter;

  uint front_facing_counter;
  uint back_facing_counter;




  uint _cpu_reserved_1;
  uint _cpu_reserved_2;

  uint _cpu_reserved_3;
  uint _cpu_reserved_4;
  uint _cpu_reserved_5;
  uint _cpu_reserved_6;

};

                                                                                                                                                                                                                                                                                                                                                                                                   






struct                 DrawPrototype {

  uint group_id;

  uint res_index;

  uint custom_id;

  uint instance_len;
};

                                                                                                                                                                          


 

































struct                 FrustumCorners {
  vec4 corners[8];
};

                                                                                                                                                                                                                                                                                        




struct                 FrustumPlanes {

  vec4 planes[6];
};

                                                                                                                                                                                                                                    




struct                 ViewCullingData {

         FrustumCorners frustum_corners;
         FrustumPlanes frustum_planes;
  vec4 bound_sphere;
};

                                                                                                                                                                                                                           




struct                 ViewMatrices {
  mat4x4 viewmat;
  mat4x4 viewinv;
  mat4x4 winmat;
  mat4x4 wininv;
};

                                                                                                                                                                                      




struct                 ObjectMatrices {
  mat4x4 model;
  mat4x4 model_inverse;

};

                                                                                                                                                            

  const uint OBJECT_SELECTED = (1u << 0u);
  const uint OBJECT_FROM_DUPLI = (1u << 1u);
  const uint OBJECT_FROM_SET = (1u << 2u);
  const uint OBJECT_ACTIVE = (1u << 3u);
  const uint OBJECT_NEGATIVE_SCALE = (1u << 4u);
  const uint OBJECT_HOLDOUT = (1u << 5u);

  const uint OBJECT_ACTIVE_EDIT_MODE = (1u << 6u);

  const uint OBJECT_NO_INFO = ~OBJECT_HOLDOUT;







                                                




struct                 ObjectInfos {

  vec3 orco_add;
  uint object_attrs_offset;
  vec3 orco_mul;
  uint object_attrs_len;

  vec4 ob_color;
  uint index;

  uint light_and_shadow_set_membership;
  float random;
       uint flag;
  float shadow_terminator_normal_offset;
  float shadow_terminator_geometry_offset;
  float _pad1;
  float _pad2;

};


                                                                                                                                                                                                                                                                                                                                                                                                                   




 

       


 




struct                 ObjectBounds {

  vec4 bounding_corners[4];

  vec4 bounding_sphere;



};

                                                                                                                                                                                                                                                          




 

       


 




struct                 VolumeInfos {

  mat4x4 grids_xform[16];

  vec4 color_mul;
  float density_scale;
  float temperature_mul;
  float temperature_bias;
  float _pad;
};

                                                                                                                                                                                                                                                            




struct                 CurvesInfos {

  uvec4 is_point_attribute[15];

  uint vertex_per_segment;

  uint half_cylinder_face_count;
  uint _pad0;
  uint _pad1;
};

                                                                                                                                                                                                                                          

#pragma pack(push, 4)



struct                 ObjectAttribute {

  float data_x;
  float data_y;
  float data_z;
  float data_w;
  uint hash_code;

};

                                                                                                                                                                                           

#pragma pack(pop)






struct                 LayerAttribute {
  vec4 data;
  uint hash_code;
  uint buffer_length;
  uint _pad1;
  uint _pad2;

};

                                                                                                                                                                                       




struct                 DrawCommandArray {
  uint vertex_len;
  uint instance_len;
  uint vertex_first;
  uint instance_first;

  uint _pad0;
  uint _pad1;
  uint _pad2;
  uint _pad3;
};

                                                                                                                                                                                                                                            




struct                 DrawCommandIndexed {
  uint vertex_len;
  uint instance_len;
  uint vertex_first;
  uint base_index;

  uint instance_first;
  uint _pad0;
  uint _pad1;
  uint _pad2;
};

                                                                                                                                                                                                                                                         





struct                 DrawCommand_union0 {
  vec4 data0;
  vec4 data1;

};

                                                                                                                                                                





struct                 DrawCommand {
         DrawCommand_union0 union0;

};


                                
                                                 
                                                                  
                                                     
                                                                      


                                                                                                                                        












 










 












 










 




struct                 DispatchCommand {
  uint num_groups_x;
  uint num_groups_y;
  uint num_groups_z;
  uint _pad0;
};

                                                                                                                                                                                     




struct                 DRWDebugVertPair {

  uint pos1_x;
  uint pos1_y;
  uint pos1_z;

  uint vert_color;

  uint pos2_x;
  uint pos2_y;
  uint pos2_z;

  uint lifetime;
};


                                                                                                                                                                                                                              




















 

       








 






struct                 DRWDebugDrawBuffer {
         DrawCommand command;
         DRWDebugVertPair verts[(2 * 1024) - 1];
};

                                                                                                                                                                                                                                       











 





























































































 layout(binding = 3, std140) uniform _drw_view_buf { ViewMatrices drw_view_buf[1]; };  






























































































 










 




 




 




 




 




 







 







 




 




 




 



 



 




 



 



 




 




 




 




 



 



 




 



 



 




 



 




 



 




 



 




 



 



 

void fullscreen_vertex(int vertex_id, inout vec4 out_position)
{
  int v = vertex_id % 3;
  float x = -1.0f + float((v & 1) << 2);
  float y = -1.0f + float((v & 2) << 1);
  out_position = vec4(x, y, 1.0f, 1.0f);
}

void fullscreen_vertex(int vertex_id, inout vec4 out_position, inout vec2 out_uv)
{
  fullscreen_vertex(vertex_id, out_position);
  out_uv = (out_position.xy + 1.0f) * 0.5f;
}

 













 






struct                 SolidLightData {
  vec4 direction;
  vec4 specular_color;
  vec4 diffuse_color_wrap;
};

                                                                                                                                                                                          




struct                 WorldData {
  vec2 viewport_size;
  vec2 viewport_size_inv;
  vec4 object_outline_color;
  vec4 shadow_direction_vs;
  float shadow_focus;
  float shadow_shift;
  float shadow_mul;
  float shadow_add;

         SolidLightData lights[4];
  vec4 ambient_color;

  int cavity_sample_start;
  int cavity_sample_end;
  float cavity_sample_count_inv;
  float cavity_jitter_scale;

  float cavity_valley_factor;
  float cavity_ridge_factor;
  float cavity_attenuation;
  float cavity_distance;

  float curvature_ridge;
  float curvature_valley;
  float ui_scale;
  float _pad0;

  int matcap_orientation;
  bool use_specular;
  float xray_alpha;
  int _pad1;

  vec4 background_color;
};

                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              




struct                 ExtrudedFrustum {

  vec4 corners[16];
  vec4 planes[12];
  int corners_count;
  int planes_count;
  int _pad0;
  int _pad1;
};

                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   




struct                 ShadowPassData {
  vec4 far_plane;
  vec3 light_direction_ws;
  int _padding;
};

                                                                                                                                                                              


 

#pragma create_info







 layout(binding = 4, std140) uniform _world_data { WorldData world_data; };  











struct workbench_World {

int _pad;};


                                        
                                       


                                                                                                             

       





 










 







 













 









 



 

#pragma create_info


















struct workbench_Cavity {

int _pad;};


                                          
                                         


                                                                                                                 

       





 























































































 







 






































 



 













 


















 


















 



 




































 



 



 









 








 







 





 



 





 



 






 



 

 




 






 








 






 





























































































































































 











 



 

#pragma create_info



















 layout(binding = 0) uniform sampler2D depth_tx;  layout(binding = 1) uniform sampler2D normal_tx;  layout(binding = 2) uniform sampler2D material_tx;  











struct workbench_resolve_Resources {

                           workbench_World  world;

                                                  workbench_Cavity  cavity;

};


                                                                
                                                               


                                                                                                                                                                                                                      

       






 




           void workbench_resolve_vert(                                                              )
{



  fullscreen_vertex(gl_VertexIndex, gl_Position);



}

struct workbench_resolve_FragOut {
                    vec4 color;
};

                                                                                                                                                            







             











































































 



void main_function_() { workbench_resolve_vert(); }
