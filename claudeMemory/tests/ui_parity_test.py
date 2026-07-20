"""P10 Phase G (T6): UI-parity regression gate.

Asserts, against the live build (GUI: keymaps only populate with a window):
1. Every vanilla "Sculpt" keymap chord is either covered by a SculptCore
   Mode chord or bound to an operator in ui-parity-allowlist.json.
2. Every vanilla sculpt Tool-tab panel (bl_context .sculpt_mode /
   .paint_common, VIEW_3D) has a SCULPTCORE_* clone registered, or is in
   the panel allowlist below.
3. The mode's menubar menus exist (Sculpt/Mask/Face Sets).

Run: blender --factory-startup --python ui_parity_test.py
  -> %TEMP%/ui_parity_test.txt (exits nonzero-equivalent via SOME FAILED)
"""
import json
import os
import tempfile

import bpy

OUT = os.path.join(tempfile.gettempdir(), "ui_parity_test.txt")
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ALLOWLIST = os.path.join(REPO, "claudeMemory", "plans", "ui-parity-allowlist.json")

# Vanilla panels with no clone, by decision (see ui-parity.md):
PANEL_ALLOWLIST = {
    "VIEW3D_PT_sculpt_dyntopo": "engine dyntopo panel replaces it (scene props)",
    "VIEW3D_PT_sculpt_voxel_remesh": "voxel remesh not in engine",
    "VIEW3D_PT_sculpt_symmetry": "mirror-only addon panel replaces it (lock/tiling gaps)",
    "VIEW3D_PT_sculpt_options": "contents are engine gaps (gravity, deform-only)",
    "VIEW3D_PT_sculpt_options_gravity": "gravity not in engine",
    "VIEW3D_PT_overlay_sculpt": "mask/face-set overlay drawing pending draw-provider work",
    "VIEW3D_PT_tools_brush_clone": "texture-paint-only (poll false in sculpt)",
    "VIEW3D_PT_tools_particlemode": "particle-edit-only (poll false in sculpt)",
}
# Chords also covered when the sculptcore side binds the same key to a
# supported equivalent op are detected via chord matching; anything else
# missing must be one of these operator prefixes from the allowlist.


def _check(lines, label, ok, detail=""):
    lines.append("{:s}: {:s}{:s}".format(
        label, "PASS" if ok else "FAIL", " ({:s})".format(detail) if detail else ""))
    return ok


def _chord(kmi):
    parts = [m for m in ("ctrl", "shift", "alt", "oskey") if getattr(kmi, m) == 1]
    if kmi.any:
        parts.append("any")
    if kmi.key_modifier != 'NONE':
        parts.append(kmi.key_modifier)
    parts.append(kmi.type)
    return "+".join(parts) + ":" + kmi.value


def main():
    lines = ["UI parity gate", ""]
    all_ok = True
    try:
        if "sculptcore_addon" not in bpy.context.preferences.addons:
            bpy.ops.preferences.addon_enable(module="sculptcore_addon")
        allow = json.load(open(ALLOWLIST))
        allow_ops = set(allow["operators"])
        allow_chords = set(allow.get("chords", ()))

        wm = bpy.context.window_manager
        vanilla = wm.keyconfigs.user.keymaps.get("Sculpt")
        ours = wm.keyconfigs.addon.keymaps.get("SculptCore Mode")
        all_ok &= _check(lines, "keymaps found", vanilla is not None and ours is not None)

        our_chords = {_chord(kmi) for kmi in ours.keymap_items}
        uncovered = [kmi for kmi in vanilla.keymap_items
                     if _chord(kmi) not in our_chords]
        offenders = [kmi.idname for kmi in uncovered
                     if kmi.idname not in allow_ops and _chord(kmi) not in allow_chords]
        all_ok &= _check(lines, "uncovered chords are allowlisted", not offenders,
                         "offenders: {!r}".format(sorted(set(offenders))) if offenders
                         else "{:d} allowlisted misses".format(len(uncovered)))

        # Vanilla sculpt Tool-tab panels: clone or allowlist.
        missing = []
        for cls_name in dir(bpy.types):
            cls = getattr(bpy.types, cls_name)
            if (getattr(cls, "bl_space_type", None) == 'VIEW_3D'
                    and getattr(cls, "bl_region_type", None) == 'UI'
                    and getattr(cls, "bl_context", "") in {".sculpt_mode", ".paint_common"}
                    and cls_name.startswith("VIEW3D_PT_")):
                clone = "SCULPTCORE_PT_" + cls_name[len("VIEW3D_PT_"):]
                if getattr(bpy.types, clone, None) is None and cls_name not in PANEL_ALLOWLIST:
                    missing.append(cls_name)
        all_ok &= _check(lines, "sculpt panels cloned or allowlisted", not missing,
                         repr(missing) if missing else "")

        for menu in ("SCULPTCORE_MT_sculpt", "SCULPTCORE_MT_mask", "SCULPTCORE_MT_face_sets"):
            all_ok &= _check(lines, "menu " + menu, getattr(bpy.types, menu, None) is not None)

        lines.append("")
        lines.append("ALL PASS" if all_ok else "SOME FAILED")
    except Exception as ex:
        import traceback
        lines.append("ERROR: {!r}".format(ex))
        lines.append(traceback.format_exc())
    with open(OUT, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    bpy.ops.wm.quit_blender()


bpy.app.timers.register(lambda: (main(), None)[1], first_interval=1.0)
