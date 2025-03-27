# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

import api


def _run(args):
    import bpy
    import time

    device_type, _ = (args['device_type'].split("-") + [""])[:2]
    scene = bpy.context.scene
    scene.render.compositor_device = ('CPU' if device_type == 'CPU' else 'GPU')
    start_time = time.time()

    bpy.ops.render.render()

    elapsed_time = time.time() - start_time
    return {'time': elapsed_time}


class CompositorTest(api.Test):
    def __init__(self, filepath):
        self.filepath = filepath

    def name(self):
        return self.filepath.stem

    def category(self):
        return "compositor"

    def use_device(self):
        return True

    def run(self, env, device_id):
        tokens = device_id.split('_')
        device_type = tokens[0]
        args = {'device_type': device_type}

        result, _ = env.run_in_blender(_run, args, [self.filepath])
        return result


def generate(env):
    filepaths = env.find_blend_files('compositor/*')
    return [CompositorTest(filepath) for filepath in filepaths]
