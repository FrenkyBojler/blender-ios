# SPDX-FileCopyrightText: 2011-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.types import Panel
from rna_prop_ui import PropertyPanel
from .space_properties import PropertiesAnimationMixin


class DataButtonsPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "data"

    @classmethod
    def poll(cls, context):
        engine = context.engine
        return context.speaker and (engine in cls.COMPAT_ENGINES)


class DATA_PT_context_speaker(DataButtonsPanel, Panel):
    bl_label = ""
    bl_options = {'HIDE_HEADER'}
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE_NEXT',
        'BLENDER_WORKBENCH',
    }

    def draw(self, context):
        layout = self.layout

        ob = context.object
        speaker = context.speaker
        space = context.space_data

        if ob:
            layout.template_ID(ob, "data")
        elif speaker:
            layout.template_ID(space, "pin_id")


class DATA_PT_speaker(DataButtonsPanel, Panel):
    bl_label = "Sound"
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE_NEXT',
        'BLENDER_WORKBENCH',
    }

    def draw_header(self, context):
        layout = self.layout
        speaker = context.speaker

        layout.prop(speaker, "muted", text="")


    def draw(self, context):
        layout = self.layout

        speaker = context.speaker
        sound = speaker.sound

        layout.template_ID(speaker, "sound", open="sound.open_mono")

        layout.use_property_split = True

        if sound is not None:
            col = layout.column()
            col.prop(sound, "filepath", text="")

            col.alignment = 'RIGHT'
            sub = col.column(align=True)
            split = sub.split(factor=0.5, align=True)
            split.alignment = 'RIGHT'
            if sound.packed_file:
                split.label(text="Unpack")
                split.operator("sound.unpack", icon='PACKAGE', text="")
            else:
                split.label(text="Pack")
                split.operator("sound.pack", icon='UGLYPACKAGE', text="")

            layout.prop(sound, "use_memory_cache")

        col = layout.column()
        col.active = not speaker.muted
        col.prop(speaker, "volume", slider=True)
        col.prop(speaker, "pitch")

        if sound is not None:
            col = layout.box()
            col = col.column(align=True)
            split = col.split(factor=0.5, align=False)
            split.alignment = 'RIGHT'
            split.label(text="Sample Rate")
            split.alignment = 'LEFT'
            if sound.samplerate <= 0:
                split.label(text="Unknown")
            else:
                split.label(text="{:d} Hz".format(sound.samplerate), translate=False)

            split = col.split(factor=0.5, align=False)
            split.alignment = 'RIGHT'
            split.label(text="Channels")
            split.alignment = 'LEFT'

            # FIXME(@campbellbarton): this is ugly, we may want to support a way of showing a label from an enum.
            channel_enum_items = sound.bl_rna.properties["channels"].enum_items
            split.label(text=channel_enum_items[channel_enum_items.find(sound.channels)].name)
            del channel_enum_items


class DATA_PT_distance(DataButtonsPanel, Panel):
    bl_label = "Distance"
    bl_parent_id = "DATA_PT_speaker"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE_NEXT',
        'BLENDER_WORKBENCH',
    }

    def draw(self, context):
        layout = self.layout

        layout.use_property_split = True

        speaker = context.speaker
        layout.active = not speaker.muted

        col = layout.column()
        sub = col.column(align=True)
        sub.prop(speaker, "volume_min", slider=True, text="Volume Min")
        sub.prop(speaker, "volume_max", slider=True, text="Max")
        col.prop(speaker, "attenuation")

        col.separator()
        col.prop(speaker, "distance_max", text="Max Distance")
        col.prop(speaker, "distance_reference", text="Distance Reference")


class DATA_PT_cone(DataButtonsPanel, Panel):
    bl_label = "Cone"
    bl_parent_id = "DATA_PT_speaker"
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE_NEXT',
        'BLENDER_WORKBENCH',
    }

    def draw(self, context):
        layout = self.layout

        layout.use_property_split = True

        speaker = context.speaker
        layout.active = not speaker.muted

        col = layout.column()

        sub = col.column(align=True)
        sub.prop(speaker, "cone_angle_outer", text="Angle Outer")
        sub.prop(speaker, "cone_angle_inner", text="Inner")

        col.separator()

        col.prop(speaker, "cone_volume_outer", slider=True)


class DATA_PT_speaker_animation(DataButtonsPanel, PropertiesAnimationMixin, PropertyPanel, Panel):
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE_NEXT',
        'BLENDER_WORKBENCH',
    }
    _animated_id_context_property = "speaker"


class DATA_PT_custom_props_speaker(DataButtonsPanel, PropertyPanel, Panel):
    COMPAT_ENGINES = {
        'BLENDER_RENDER',
        'BLENDER_EEVEE_NEXT',
        'BLENDER_WORKBENCH',
    }
    _context_path = "object.data"
    _property_type = bpy.types.Speaker


classes = (
    DATA_PT_context_speaker,
    DATA_PT_speaker,
    DATA_PT_distance,
    DATA_PT_cone,
    DATA_PT_speaker_animation,
    DATA_PT_custom_props_speaker,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
