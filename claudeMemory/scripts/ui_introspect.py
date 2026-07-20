"""UI parity T2: capture what sculpt panels/menus actually draw, as data.

`UILayout.introspect()` returns a JSON description of a layout's contents,
but real layouts only exist inside draw callbacks. Trick: register a capture
panel in the View3D sidebar (own category, forced active via
Region.active_panel_category) and, inside its draw, call each target
Panel/Menu class's draw() against a shim object whose `.layout` is a fresh
sub-layout of the capture panel's layout. The target never needs to be
visible, bl_context-matched, or even pollable — only the context (mode,
active object, brush) must be right. Introspection of the sub-layout then
yields the panel's item list: operators, props, labels, order.
(A panel layout is required: header layouts reject `layout.panel()`
sub-panels, which brush panels use.)

Captured per job (mode x class):
- vanilla sculpt panels/menus/pies under vanilla SCULPT mode -> the parity
  spec our addon UI is diffed against;
- the addon's SCULPTCORE_PT_* panels under the CUSTOM mode;
- VIEW3D_HT_header and VIEW3D_MT_editor_menus under both modes (their draw
  branches by mode, so the diff shows header/menubar parity).

Draw errors are recorded per job, not fatal: a vanilla panel erroring under
CUSTOM-mode context is itself a finding, and pie menus may reject
`menu_pie()` outside a real pie popup.

Run (GUI required):
  set SCULPTCORE_PYTHON_PATH=<repo>/extern/sculptcore/python
  blender.exe --factory-startup --python claudeMemory/scripts/ui_introspect.py
    -> %TEMP%/ui_introspect.txt (summary) + %TEMP%/ui_introspect.json (data)
"""
import inspect
import json
import os
import tempfile
import types

import bpy

OUT = os.path.join(tempfile.gettempdir(), "ui_introspect.txt")
OUT_JSON = os.path.join(tempfile.gettempdir(), "ui_introspect.json")

# Class-name prefixes of the vanilla sculpt UI surface (panels found by the
# T1 poll matrix, plus the sculpt menus/pies and the context menu).
VANILLA_PREFIXES = (
    "VIEW3D_PT_sculpt",            # dyntopo, symmetry, options, remesh, context menu
    "VIEW3D_PT_overlay_sculpt",
    "VIEW3D_PT_tools_brush",       # brush settings/stroke/falloff/texture/display
    "VIEW3D_MT_sculpt",            # Sculpt menu, trim/project submenus, pies
    "VIEW3D_MT_mask",
    "VIEW3D_MT_face_sets",
    "VIEW3D_MT_mesh_paint_automasking_pie",
)
# Mode-dependent surfaces captured under BOTH modes for direct diffing.
BOTH_MODES = ("VIEW3D_HT_header", "VIEW3D_MT_editor_menus")

MAX_TICKS = 100

pending = []    # (mode_label, class_name) jobs for the draw hook
results = {}    # "MODE/ClassName" -> {ok, items|error}
state = {"phase": "init", "ticks": 0}


class _Shim:
    """Stand-in for a Panel/Menu instance: real sub-layout, class attributes.

    Instance methods resolve through the class and are bound to the shim, so
    mixin helpers (e.g. View3DPaintPanel.paint_settings) work. RNA getset
    descriptors that need a real instance are shadowed by plain defaults.
    """

    def __init__(self, cls, layout):
        self._cls = cls
        self.layout = layout
        self.is_popover = False
        self.is_extended = False
        self.text = ""

    def __getattr__(self, name):
        # The raw descriptor decides the binding: plain functions bind to the
        # shim, staticmethods stay unbound, classmethods bind to the class.
        raw = inspect.getattr_static(self._cls, name)
        if isinstance(raw, staticmethod):
            return raw.__func__
        if isinstance(raw, types.FunctionType):
            return types.MethodType(raw, self)
        if isinstance(raw, property):
            return raw.fget(self)
        return getattr(self._cls, name)


class SCULPTCORE_PT_ui_capture(bpy.types.Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    # "Item" is the sidebar's default-active tab under --factory-startup, and
    # Region.active_panel_category is read-only from Python, so ride along in
    # the tab that is already visible.
    bl_category = "Item"
    bl_label = "UI Capture"

    def draw(self, context):
        while pending:
            mode_label, name = pending.pop(0)
            key = "{:s}/{:s}".format(mode_label, name)
            cls = getattr(bpy.types, name, None)
            if cls is None:
                results[key] = {"ok": False, "error": "class not registered"}
                continue
            col = self.layout.column()
            try:
                cls.draw(_Shim(cls, col), context)
                raw = col.introspect()
                try:
                    items = json.loads(raw)
                except Exception:
                    items = raw
                results[key] = {"ok": True, "items": items}
            except Exception as ex:
                results[key] = {"ok": False, "error": repr(ex)}


def _show_capture_sidebar():
    for area in bpy.context.window_manager.windows[0].screen.areas:
        if area.type != 'VIEW_3D':
            continue
        area.spaces.active.show_region_ui = True


def _targets(prefixes):
    names = set()
    for base in (bpy.types.Panel, bpy.types.Menu):
        stack = [base]
        while stack:
            cls = stack.pop()
            stack.extend(cls.__subclasses__())
            if getattr(cls, "is_registered", False) and cls.__name__.startswith(prefixes):
                names.add(cls.__name__)
    return sorted(names)


def _tag_view3d_redraw():
    for area in bpy.context.window_manager.windows[0].screen.areas:
        if area.type == 'VIEW_3D':
            area.tag_redraw()


def _finish(note=""):
    lines = ["UI introspection capture", ""]
    if note:
        lines.append(note)
    ok = sorted(k for k, v in results.items() if v["ok"])
    bad = sorted(k for k, v in results.items() if not v["ok"])
    lines.append("captured: {:d}   errors: {:d}".format(len(ok), len(bad)))
    lines.append("")
    for key in ok:
        items = results[key]["items"]
        count = len(items) if isinstance(items, list) else "?"
        lines.append("  OK   {:s} ({!s} top-level items)".format(key, count))
    for key in bad:
        lines.append("  FAIL {:s}: {:s}".format(key, results[key]["error"]))
    lines.append("")
    lines.append("full data: {:s}".format(OUT_JSON))
    with open(OUT_JSON, "w") as f:
        json.dump(results, f, indent=1)
    with open(OUT, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    bpy.ops.wm.quit_blender()


def _tick():
    state["ticks"] += 1
    if state["ticks"] > MAX_TICKS:
        _finish("TIMEOUT in phase {!r}; {:d} jobs left".format(state["phase"], len(pending)))
        return None
    try:
        if state["phase"] == "init":
            bpy.ops.object.select_all(action='SELECT')
            bpy.ops.object.delete()
            bpy.ops.mesh.primitive_uv_sphere_add(segments=32, ring_count=16)
            if "sculptcore_addon" not in bpy.context.preferences.addons:
                bpy.ops.preferences.addon_enable(module="sculptcore_addon")
            bpy.ops.object.mode_set(mode='SCULPT')
            for name in _targets(VANILLA_PREFIXES) + list(BOTH_MODES):
                pending.append(("SCULPT", name))
            bpy.utils.register_class(SCULPTCORE_PT_ui_capture)
            _show_capture_sidebar()
            state["phase"] = "sculpt"
            _tag_view3d_redraw()
        elif state["phase"] == "sculpt":
            if pending:
                _tag_view3d_redraw()
            else:
                bpy.ops.object.mode_set(mode='OBJECT')
                bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
                for name in _targets(("SCULPTCORE_PT_",)) + list(BOTH_MODES):
                    if name != SCULPTCORE_PT_ui_capture.__name__:
                        pending.append(("CUSTOM", name))
                state["phase"] = "custom"
                _tag_view3d_redraw()
        elif state["phase"] == "custom":
            if pending:
                _tag_view3d_redraw()
            else:
                _finish()
                return None
    except Exception as ex:
        import traceback
        _finish("ERROR: {!r}\n{:s}".format(ex, traceback.format_exc()))
        return None
    return 0.1


bpy.app.timers.register(_tick, first_interval=1.0)
