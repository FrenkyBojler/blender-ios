"""UI parity T4: deterministic screenshots of the UI in both sculpt modes.

Sets up a deterministic scene (factory startup, one sphere, view_all,
Properties editor on the Tool tab, View3D sidebar open), then captures per
mode (vanilla SCULPT, SculptCore CUSTOM):
- the full window        -> ui_shot_<MODE>_full.png
- the View3D area crop   -> ui_shot_<MODE>_view3d.png
- the Properties crop    -> ui_shot_<MODE>_properties.png

Crops are cut from the full screenshot with numpy using the area rects
(scaled by image/window size ratio, so Windows DPI scaling is handled).
Complements T1/T2: shows icons, enabled/greyed state, and layout density
that introspection cannot.

Known limits: the View3D sidebar shows its default-active category tab
("Item"; Region.active_panel_category is read-only from Python), and the
brush cursor sits wherever the OS mouse happens to be.

Run (GUI required; pass a fixed geometry for comparable shots across runs):
  set SCULPTCORE_PYTHON_PATH=<repo>/extern/sculptcore/python
  blender.exe --factory-startup --window-geometry 0 0 1600 1000 \
      --python claudeMemory/scripts/ui_screenshot.py
    -> %TEMP%/ui_screenshot.txt (manifest) + %TEMP%/ui_shot_*.png
"""
import os
import tempfile

import bpy
import numpy as np

OUT = os.path.join(tempfile.gettempdir(), "ui_screenshot.txt")

MODES = ("SCULPT", "CUSTOM")
SETTLE_TICKS = 3  # redraws to wait after a mode switch before shooting
MAX_TICKS = 100

state = {"phase": "init", "mode_index": 0, "settle": 0, "ticks": 0}
manifest = []

# Runs before the first window draw, so the splash never appears.
bpy.context.preferences.view.show_splash = False


def _shot_path(mode, tag):
    return os.path.join(tempfile.gettempdir(),
                        "ui_shot_{:s}_{:s}.png".format(mode, tag))


def _window():
    return bpy.context.window_manager.windows[0]


def _tag_redraw_all():
    for area in _window().screen.areas:
        area.tag_redraw()


def _setup_scene():
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()
    bpy.ops.mesh.primitive_uv_sphere_add(segments=32, ring_count=16)
    bpy.ops.object.shade_smooth()
    if "sculptcore_addon" not in bpy.context.preferences.addons:
        bpy.ops.preferences.addon_enable(module="sculptcore_addon")
    for area in _window().screen.areas:
        if area.type == 'VIEW_3D':
            area.spaces.active.show_region_ui = True
            region = next(r for r in area.regions if r.type == 'WINDOW')
            with bpy.context.temp_override(window=_window(), area=area, region=region):
                bpy.ops.view3d.view_all(center=True)
        elif area.type == 'PROPERTIES':
            area.spaces.active.context = 'TOOL'


def _crop_area(full_path, area, out_path):
    window = _window()
    img = bpy.data.images.load(full_path)
    width, height = img.size
    px = np.array(img.pixels[:], dtype=np.float32).reshape(height, width, 4)
    bpy.data.images.remove(img)

    sx = width / window.width
    sy = height / window.height
    x0, y0 = int(area.x * sx), int(area.y * sy)
    x1 = min(int((area.x + area.width) * sx), width)
    y1 = min(int((area.y + area.height) * sy), height)
    crop = px[y0:y1, x0:x1]

    out = bpy.data.images.new("crop", width=x1 - x0, height=y1 - y0, alpha=True)
    out.pixels.foreach_set(np.ascontiguousarray(crop.reshape(-1)))
    out.filepath_raw = out_path
    out.file_format = 'PNG'
    out.save()
    bpy.data.images.remove(out)


def _capture(mode):
    full = _shot_path(mode, "full")
    bpy.ops.screen.screenshot(filepath=full)
    manifest.append(full)
    for area in _window().screen.areas:
        tag = {'VIEW_3D': "view3d", 'PROPERTIES': "properties"}.get(area.type)
        if tag is not None:
            path = _shot_path(mode, tag)
            _crop_area(full, area, path)
            manifest.append(path)


def _enter(mode):
    if mode == 'CUSTOM':
        if bpy.context.active_object.mode != 'OBJECT':
            bpy.ops.object.mode_set(mode='OBJECT')
        bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
    else:
        bpy.ops.object.mode_set(mode=mode)


def _finish(note=""):
    lines = ["UI screenshots", ""]
    if note:
        lines.append(note)
    lines.extend(manifest)
    with open(OUT, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    bpy.ops.wm.quit_blender()


def _tick():
    state["ticks"] += 1
    if state["ticks"] > MAX_TICKS:
        _finish("TIMEOUT in phase {!r}".format(state["phase"]))
        return None
    try:
        if state["phase"] == "init":
            _setup_scene()
            _enter(MODES[0])
            state.update(phase="settle", settle=0)
            _tag_redraw_all()
        elif state["phase"] == "settle":
            state["settle"] += 1
            _tag_redraw_all()
            if state["settle"] >= SETTLE_TICKS:
                state["phase"] = "shoot"
        elif state["phase"] == "shoot":
            mode = MODES[state["mode_index"]]
            _capture(mode)
            state["mode_index"] += 1
            if state["mode_index"] >= len(MODES):
                _finish()
                return None
            _enter(MODES[state["mode_index"]])
            state.update(phase="settle", settle=0)
            _tag_redraw_all()
    except Exception as ex:
        import traceback
        _finish("ERROR: {!r}\n{:s}".format(ex, traceback.format_exc()))
        return None
    return 0.2


bpy.app.timers.register(_tick, first_interval=1.0)
