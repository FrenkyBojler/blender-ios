"""Run a p8 harness script in a GUI Blender and quit once it writes its output.
Usage: blender --factory-startup --python run_harness.py -- <script> <outname>
"""
import os
import sys
import tempfile

import bpy

argv = sys.argv[sys.argv.index("--") + 1:]
script_path, out_name = argv[0], argv[1]
out_file = os.path.join(tempfile.gettempdir(), out_name)
try:
    os.remove(out_file)
except OSError:
    pass

exec(compile(open(script_path).read(), script_path, "exec"))


def _quit():
    if os.path.exists(out_file):
        bpy.ops.wm.quit_blender()
        return None
    return 1.0


bpy.app.timers.register(_quit, first_interval=3.0)
