import bpy
tool_settings = bpy.context.scene.tool_settings

tool_settings.snap_elements = {'INCREMENT'}
tool_settings.snap_target = 'CLOSEST'
tool_settings.snap_elements_base = {'INCREMENT'}
tool_settings.snap_elements_individual = set()
tool_settings.use_snap_grid_absolute = False
tool_settings.use_snap_peel_object = False
tool_settings.use_snap_to_same_target = False
tool_settings.snap_face_nearest_steps = 1
tool_settings.use_snap_align_rotation = False
tool_settings.use_snap_backface_culling = False
tool_settings.use_snap_self = True
tool_settings.use_snap_edit = True
tool_settings.use_snap_nonedit = True
tool_settings.use_snap_selectable = False
tool_settings.use_snap_translate = True
tool_settings.use_snap_rotate = False
tool_settings.use_snap_scale = False
tool_settings.snap_angle_increment_3d = 0.0872665
tool_settings.snap_angle_increment_3d_precision = 0.0174533
