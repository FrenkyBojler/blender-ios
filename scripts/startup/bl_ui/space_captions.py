    # SPDX-FileCopyrightText: 2009-2023 Blender Authors
    #
    # SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.types import Header, Menu, Panel, PropertyGroup
from bpy.props import IntProperty, StringProperty
from bpy.app.translations import (
    contexts as i18n_contexts,
    pgettext_iface as iface_,
)

class CAPTIONS_PT_main(bpy.types.Panel):
    bl_idname = "CAPTIONS_PT_main"
    bl_label = "Main Panel"
    bl_space_type = 'CAPTIONS_EDITOR'
    bl_region_type = 'WINDOW'
    bl_options = {'HIDE_HEADER'}
    
    def draw(self, context):
        pass
    
class CAPTIONS_PT_style(bpy.types.Panel):
    bl_idname = "CAPTIONS_PT_style"
    bl_label = "Style Options"
    bl_space_type = 'CAPTIONS_EDITOR'
    bl_region_type = 'WINDOW'
    bl_parent_id = "CAPTIONS_PT_main"
    #bl_options = {'HIDE_HEADER'}
    bl_order = 0
    
    def draw(self, context):
        # TODO: Add style options for captions here
        space = context.space_data
        scene = context.scene
        editor = scene.sequence_editor
        strips = editor.captions_strips
        
        style = editor.captions_style
        if(style is None):
            return
        
        layout = self.layout
         
        # Draw the style as a strip properties, works becuase they're both using TextVars data, but it's hacky.
        from bpy.types import (
            STRIP_PT_effect_text_style,
            STRIP_PT_effect_text_outline,
            STRIP_PT_effect_text_shadow,
            STRIP_PT_effect_text_box,
            STRIP_PT_effect_text_layout
        )
        
        STRIP_PT_effect_text_style.draw_effect_text_style(style, layout)
        
        header, body = layout.panel("outline", default_closed=True)
        header.label(text="Outline")
        header.prop(style, "use_outline", text="")
        if body:
            STRIP_PT_effect_text_outline.draw_effect_text_outline(style, body)
        
        header, body = layout.panel("shadow", default_closed=True)
        header.label(text="Shadow")
        header.prop(style, "use_shadow", text="")
        if body:
            STRIP_PT_effect_text_shadow.draw_effect_text_shadow(style, body)
        
        header, body = layout.panel("box", default_closed=True)
        header.label(text="Box")
        header.prop(style, "use_box", text="")
        if body:
            STRIP_PT_effect_text_box.draw_effect_text_box(style, body)  
           
        header, body = layout.panel("layout", default_closed=True)
        header.label(text="Layout")
        if body:
            STRIP_PT_effect_text_layout.draw_effect_text_layout(style, body)
        
class CAPTIONS_PT_list(bpy.types.Panel):
    bl_idname = "CAPTIONS_PT_list"
    bl_label = "Style Options"
    bl_space_type = 'CAPTIONS_EDITOR'
    bl_region_type = 'WINDOW'
    bl_parent_id = "CAPTIONS_PT_main"
    bl_options = {'HIDE_HEADER'}

    def draw_caption(self, layout, item):
        split = layout.split(factor=0.35)
        col1 = split.column(align=True)
        col1.prop(item, "frame_start", text="")
        col1.prop(item, "frame_final_end", text="")
        
        col2 = split.column()
        col2.scale_y = 2
        col2.prop(item, "text", text="")

    def draw(self, context):
        layout = self.layout
        space = context.space_data
        strips = context.scene.sequence_editor.captions_strips
        
        # Iterate over the collection property
        for strip in strips:
            self.draw_caption(layout, strip)
        
        layout.operator("captions.caption_add", text="Add", icon='ADD')
        
class CAPTIONS_HT_header(Header):
    bl_space_type = 'CAPTIONS_EDITOR'
    bl_region_type = 'HEADER'
    def draw(self, context):
        layout = self.layout
        st = context.space_data
        layout.template_header()
        layout.separator_spacer()
class CAPTIONS_MT_context_menu(Menu):
    bl_label = ""
    def draw(self, _context):
        layout = self.layout
        layout.operator_context = 'INVOKE_DEFAULT'
        
classes = (
    CAPTIONS_PT_main,
    CAPTIONS_PT_style,
    CAPTIONS_PT_list,
    CAPTIONS_HT_header,
    CAPTIONS_MT_context_menu,
)
def register():
    for cls in classes:
        bpy.utils.register_class(cls)
        
def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
        
if __name__ == "__main__":
    register()
