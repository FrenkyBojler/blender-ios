"""UI parity T1: panel/menu/header poll matrix across sculpt-mode variants.

Enumerates every registered Panel/Menu/Header subclass and records its
registration metadata (space, region, category, bl_context, parent, options)
plus the result of poll() under three modes on the same mesh object:
OBJECT (baseline), vanilla SCULPT, and the SculptCore custom mode. The
JSON diff of the SCULPT and CUSTOM columns is the work list of UI surfaces
vanilla sculpt mode has that the custom mode lacks.

Caveats baked into the data (analyze, don't ignore):
- poll() True does not mean visible. VIEW_3D/PROPERTIES panels are also
  gated by bl_context matching the mode-derived context string before poll
  is even called, so a panel with bl_context "sculpt_mode"/".paint_common"
  that polls True under CUSTOM may still never draw there. bl_context is
  recorded for exactly this reason.
- Menus mostly have no poll (True everywhere); whether they appear depends
  on who references them (T2's job).

Run: blender --factory-startup --python ui_poll_matrix.py
  -> %TEMP%/ui_poll_matrix.txt (summary) + %TEMP%/ui_poll_matrix.json (full)
"""
import json
import os
import tempfile

import bpy

OUT = os.path.join(tempfile.gettempdir(), "ui_poll_matrix.txt")
OUT_JSON = os.path.join(tempfile.gettempdir(), "ui_poll_matrix.json")

MODES = ("OBJECT", "SCULPT", "CUSTOM")


def _all_subclasses(cls, seen=None):
    if seen is None:
        seen = set()
    for sub in cls.__subclasses__():
        if sub not in seen:
            seen.add(sub)
            _all_subclasses(sub, seen)
    return seen


def _registered(base):
    return sorted(
        (c for c in _all_subclasses(base) if getattr(c, "is_registered", False)),
        key=lambda c: c.__name__)


def _meta(cls, kind):
    options = getattr(cls, "bl_options", None)
    return {
        "class": cls.__name__,
        "kind": kind,
        "idname": getattr(cls, "bl_idname", cls.__name__),
        "label": getattr(cls, "bl_label", ""),
        "space": getattr(cls, "bl_space_type", ""),
        "region": getattr(cls, "bl_region_type", ""),
        "category": getattr(cls, "bl_category", ""),
        "context": getattr(cls, "bl_context", ""),
        "parent": getattr(cls, "bl_parent_id", ""),
        "options": sorted(options) if options else [],
        "module": cls.__module__,
        "poll": {},
    }


def _area_region(window, space_type, region_type):
    """Best-effort (area, region) pair on the current screen; Nones if absent."""
    for area in window.screen.areas:
        if space_type and area.type != space_type:
            continue
        for region in area.regions:
            if region.type == region_type:
                return area, region
        return area, None
    return None, None


def _poll_one(window, cls, meta):
    space = meta["space"] or 'VIEW_3D'
    region_type = meta["region"] or 'WINDOW'
    area, region = _area_region(window, space, region_type)
    if area is None:
        return "NO_AREA"
    try:
        override = {"window": window, "area": area}
        if region is not None:
            override["region"] = region
        with bpy.context.temp_override(**override):
            fn = getattr(cls, "poll", None)
            if fn is None:
                return True
            return bool(cls.poll(bpy.context))
    except AttributeError:
        # Polls may assume context members (context.bone, context.fluid, ...)
        # that the panel system's bl_context prefilter would have guaranteed;
        # a miss means "not applicable here", not a failure.
        return "NA"
    except Exception as ex:
        return "ERROR: {!r}".format(ex)


def _collect(window, rows):
    for meta, cls in rows:
        yield meta, _poll_one(window, cls, meta)


def _properties_tab_to_tool(window):
    """The Tool tab is where paint brush panels live in the Properties editor."""
    for area in window.screen.areas:
        if area.type == 'PROPERTIES':
            try:
                area.spaces.active.context = 'TOOL'
                return True
            except Exception:
                return False
    return False


def main():
    lines = ["UI poll matrix", ""]
    try:
        context = bpy.context
        window = context.window_manager.windows[0]

        bpy.ops.object.select_all(action='SELECT')
        bpy.ops.object.delete()
        bpy.ops.mesh.primitive_uv_sphere_add(segments=32, ring_count=16)

        if "sculptcore_addon" not in context.preferences.addons:
            bpy.ops.preferences.addon_enable(module="sculptcore_addon")

        rows = []
        for base, kind in ((bpy.types.Panel, "Panel"),
                           (bpy.types.Menu, "Menu"),
                           (bpy.types.Header, "Header")):
            for cls in _registered(base):
                rows.append((_meta(cls, kind), cls))

        tool_tab = _properties_tab_to_tool(window)
        mode_strings = {}
        for mode in MODES:
            if mode == 'CUSTOM':
                bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
            elif bpy.context.active_object.mode != mode:
                bpy.ops.object.mode_set(mode=mode)
            mode_strings[mode] = bpy.context.mode
            for meta, result in _collect(window, rows):
                meta["poll"][mode] = result
            if mode == 'CUSTOM':
                bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")

        data = {
            "mode_strings": mode_strings,
            "properties_tool_tab": tool_tab,
            "rows": [meta for meta, _cls in rows],
        }
        with open(OUT_JSON, "w") as f:
            json.dump(data, f, indent=1)

        # Summary: sculpt-mode surfaces the custom mode does not poll.
        missing = [m for m, _c in rows
                   if m["poll"].get("SCULPT") is True and m["poll"].get("CUSTOM") is not True]
        sculpt_only = [m for m in missing if m["poll"].get("OBJECT") is not True]
        errors = [m for m, _c in rows
                  if any(isinstance(v, str) and v.startswith("ERROR") for v in m["poll"].values())]
        extra = [m for m, _c in rows
                 if "sculptcore" in m["module"] and m["poll"].get("CUSTOM") is True]

        lines.append("context.mode strings: {!r}".format(mode_strings))
        lines.append("classes: {:d}   sculpt-True-but-not-custom: {:d} "
                     "(sculpt-specific: {:d})   poll errors: {:d}".format(
                         len(rows), len(missing), len(sculpt_only), len(errors)))
        lines.append("")
        lines.append("== Sculpt-specific surfaces missing in CUSTOM ==")
        for m in sorted(sculpt_only, key=lambda m: (m["space"], m["region"], m["class"])):
            lines.append("  {:s} {:s}/{:s} cat={!r} ctx={!r} parent={!r} {:s} [{:s}]".format(
                m["kind"], m["space"], m["region"], m["category"], m["context"],
                m["parent"], m["class"], m["module"]))
        lines.append("")
        lines.append("== Addon surfaces active in CUSTOM ({:d}) ==".format(len(extra)))
        for m in extra:
            lines.append("  {:s} {:s}/{:s} cat={!r} {:s}".format(
                m["kind"], m["space"], m["region"], m["category"], m["class"]))
        if errors:
            lines.append("")
            lines.append("== Poll errors ==")
            for m in errors:
                lines.append("  {:s}: {!r}".format(m["class"], m["poll"]))
        lines.append("")
        lines.append("full data: {:s}".format(OUT_JSON))
    except Exception as ex:
        import traceback
        lines.append("ERROR: {!r}".format(ex))
        lines.append(traceback.format_exc())
    with open(OUT, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    bpy.ops.wm.quit_blender()


bpy.app.timers.register(lambda: (main(), None)[1], first_interval=1.0)
