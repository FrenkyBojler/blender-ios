# SPDX-FileCopyrightText: 2011-2022 Blender Foundation
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

from bl_operators.presets import AddPresetBase
from bpy.types import Operator


class AddPresetIntegrator(AddPresetBase, Operator):
    '''Add an Integrator Preset'''
    bl_idname = "render.cycles_integrator_preset_add"
    bl_label = "Add Integrator Preset"
    preset_menu = "CYCLES_PT_integrator_presets"

    preset_defines = [
        "cycles = bpy.context.scene.cycles"
    ]

    preset_values = [
        "cycles.max_bounces",
        "cycles.diffuse_bounces",
        "cycles.glossy_bounces",
        "cycles.transmission_bounces",
        "cycles.volume_bounces",
        "cycles.transparent_max_bounces",
        "cycles.caustics_reflective",
        "cycles.caustics_refractive",
        "cycles.blur_glossy",
        "cycles.use_fast_gi",
        "cycles.ao_bounces",
        "cycles.ao_bounces_render",
    ]

    preset_subdir = "cycles/integrator"


class AddPresetSampling(AddPresetBase, Operator):
    '''Add a Sampling Preset'''
    bl_idname = "render.cycles_sampling_preset_add"
    bl_label = "Add Sampling Preset"
    preset_menu = "CYCLES_PT_sampling_presets"

    preset_defines = [
        "cycles = bpy.context.scene.cycles"
    ]

    preset_values = [
        "cycles.use_adaptive_sampling",
        "cycles.samples",
        "cycles.adaptive_threshold",
        "cycles.adaptive_min_samples",
        "cycles.time_limit",
        "cycles.use_denoising",
        "cycles.denoiser",
        "cycles.denoising_input_passes",
        "cycles.denoising_prefilter",
        "cycles.denoising_quality",
    ]

    preset_subdir = "cycles/sampling"


class AddPresetViewportSampling(AddPresetBase, Operator):
    '''Add a Viewport Sampling Preset'''
    bl_idname = "render.cycles_viewport_sampling_preset_add"
    bl_label = "Add Viewport Sampling Preset"
    preset_menu = "CYCLES_PT_viewport_sampling_presets"

    preset_defines = [
        "cycles = bpy.context.scene.cycles"
    ]

    preset_values = [
        "cycles.use_preview_adaptive_sampling",
        "cycles.preview_samples",
        "cycles.preview_adaptive_threshold",
        "cycles.preview_adaptive_min_samples",
        "cycles.use_preview_denoising",
        "cycles.preview_denoiser",
        "cycles.preview_denoising_input_passes",
        "cycles.preview_denoising_prefilter",
        "cycles.preview_denoising_quality",
        "cycles.preview_denoising_start_sample",
    ]

    preset_subdir = "cycles/viewport_sampling"


class AddPresetPerformance(AddPresetBase, Operator):
    '''Add an Performance Preset'''
    bl_idname = "render.cycles_performance_preset_add"
    bl_label = "Add Performance Preset"
    preset_menu = "CYCLES_PT_performance_presets"

    preset_defines = [
        "render = bpy.context.scene.render",
        "cycles = bpy.context.scene.cycles",
    ]

    preset_values = [
        "render.threads_mode",
        "render.use_persistent_data",
        "cycles.debug_use_spatial_splits",
        "cycles.debug_use_compact_bvh",
        "cycles.debug_use_hair_bvh",
        "cycles.debug_bvh_time_steps",
        "cycles.tile_size",
    ]

    preset_subdir = "cycles/performance"


class AddPresetLPE(AddPresetBase, Operator):
    '''Add a Light Path Expression Preset'''
    bl_idname = "render.cycles_lpe_preset_add"
    bl_label = "Add LPE Preset"
    preset_menu = "CYCLES_PT_lpe_presets"

    preset_defines = [
        "view_layer = bpy.context.view_layer",
    ]

    preset_values = [
        "view_layer.active_lpe.name",
        "view_layer.active_lpe.expression",
    ]

    preset_subdir = "cycles/light_path_expressions"

    @classmethod
    def poll(cls, context):
        view_layer = context.view_layer
        return view_layer and view_layer.active_lpe

    def add(self, context, filepath):
        import os

        view_layer = context.view_layer
        lpe = view_layer.active_lpe

        if not lpe:
            return

        # Use the preset name from user input instead of lpe.name
        preset_name = os.path.splitext(os.path.basename(filepath))[0]

        with open(filepath, "w", encoding="utf-8") as file_preset:
            file_preset.write("import bpy\n")
            file_preset.write("view_layer = bpy.context.view_layer\n\n")
            file_preset.write("view_layer.lpes.add()\n")
            file_preset.write(f"view_layer.active_lpe.name = {repr(preset_name)}\n")
            file_preset.write(f"view_layer.active_lpe.expression = {repr(lpe.expression)}\n")


classes = (
    AddPresetIntegrator,
    AddPresetSampling,
    AddPresetViewportSampling,
    AddPresetPerformance,
    AddPresetLPE,
)


def register():
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)


def unregister():
    from bpy.utils import unregister_class
    for cls in classes:
        unregister_class(cls)


if __name__ == "__main__":
    register()
