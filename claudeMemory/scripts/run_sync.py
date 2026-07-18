"""Run a synchronous test script (main() at import) and quit Blender.
Usage: blender --factory-startup --python run_sync.py -- <test.py>
"""
import os
import sys
import traceback

import bpy

path = sys.argv[sys.argv.index("--") + 1]
g = {"__name__": "__main__", "__file__": path}
try:
    exec(compile(open(path).read(), path, "exec"), g)
except Exception:
    print("TEST FAILED WITH EXCEPTION:")
    traceback.print_exc()
# Hard exit: wm.quit_blender can block on the save prompt, and these
# regression runs need no teardown.
sys.stdout.flush()
sys.stderr.flush()
os._exit(0)
