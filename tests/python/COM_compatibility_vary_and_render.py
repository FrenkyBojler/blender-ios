import bpy

IGNORE_ATTR = set(('__doc__', '__module__', '__slots__', 'bl_description',
                  'bl_height_default', 'bl_height_max', 'bl_height_min',
                  'bl_icon', 'bl_idname', 'bl_label', 'bl_rna', 'bl_static_type',
                  'bl_width_default', 'bl_width_max', 'bl_width_min', 'color',
                  'color_tag', 'debug_zone_body_lazy_function_graph',
                  'debug_zone_lazy_function_graph', 'dimensions', 'draw_buttons',
                  'draw_buttons_ext', 'height', 'hide', 'input_template', 'inputs',
                  'internal_links', 'is_registered_node_type', 'label', 'location',
                  'location_absolute', 'mute', 'name', 'output_template', 'outputs',
                  'parent', 'poll', 'poll_instance', 'rna_type', 'select', 'show_options',
                  'show_preview', 'show_texture', 'socket_idname', 'socket_value_update',
                  'type', 'update', 'use_custom_color', 'warning_propagation', 'width', 'tag_need_exec'))

TEST_NODES = set(["CompositorNodeBokehImage",
                  "CompositorNodeTime",
                  "CompositorNodeMask",
                  "CompositorNodeSplit",
                  "CompositorNodeTonemap",
                  "CompositorNodeDilateErode",
                  "CompositorNodeInpaint",
                  ])

for node in bpy.data.scenes[0].node_tree.nodes:
    print()
    print("node:", node.name)
    print("node.bl_idname:", node.bl_idname)
    if node.bl_idname not in TEST_NODES:
        continue

    for attribute in dir(node):
        print("\tattribute:", attribute)
        if attribute in IGNORE_ATTR:
            continue

        val = getattr(node, attribute)
        if not (isinstance(val, int) or isinstance(val, float)):
            continue

        bpy.data.scenes[0].frame_current = 1
        node.keyframe_insert(data_path = attribute)
        if isinstance(val, int) or isinstance(val, float):
            if val == 0:
                setattr(node, attribute, 1)
            else:
                if isinstance(val, int):
                    new_val = int(val + 0.5 * val)
                else:
                    new_val = float(val + 0.5 * val)
                setattr(node, attribute, new_val)
            bpy.data.scenes[0].frame_current = 10
            node.keyframe_insert(data_path = attribute)

bpy.data.scenes[0].frame_current = 5
bpy.data.scenes[0].render.image_settings.file_format = 'OPEN_EXR'
render_name = bpy.data.filepath.replace(".blend", "")
blendfile_name = bpy.data.filepath.replace(".blend", "_compat_test.blend")
print("#### render_name", render_name)
bpy.data.scenes[0].render.filepath = render_name
bpy.ops.render.render(write_still=True)
bpy.ops.wm.save_mainfile(filepath=blendfile_name)



