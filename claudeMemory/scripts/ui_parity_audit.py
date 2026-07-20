"""P10 Phase 0: operator/capability audit generator.

Reads the T2/T3 dumps (%TEMP%/ui_introspect.json, keymap_dump.json), extracts
every operator the vanilla sculpt UI references, joins each with the
classification table below, and writes:
- claudeMemory/plans/ui-parity-audit.md      (the audit table)
- claudeMemory/plans/ui-parity-allowlist.json (class-3 entries; consumed by T6)

Classification (plan P10):
1 = works as-is: shared state (tool_settings.sculpt / brush assets) or pure
    WM plumbing; at most needs re-verification inside the custom mode.
2 = needs a sculptcore.* addon operator over an existing engine capability.
3 = deferred: the engine lacks the underlying feature (or it is out of
    scope); recorded in the allowlist so T6 treats the gap as intentional.

The table is maintained by hand, grounded in engine evidence (mapping.py,
convert.py, the p8 harness scripts). Operators found in the dumps but not in
the table are emitted as UNCLASSIFIED — a rerun after Blender adds sculpt UI
surfaces flags the new work instead of silently passing.

Run: python claudeMemory/scripts/ui_parity_audit.py   (plain Python, no bpy)
"""
import collections
import json
import os
import re
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PLANS = os.path.join(REPO, "claudeMemory", "plans")
AUDIT_MD = os.path.join(PLANS, "ui-parity-audit.md")
ALLOWLIST = os.path.join(PLANS, "ui-parity-allowlist.json")

# op idname -> (class, note). Difficulty tags on class 2: easy/moderate.
CLASSIFY = {
    # -- 1: works as-is (shared state / WM); verify in-mode, don't rewrite --
    "brush.asset_activate": (1, "brush assets act on the shared sculpt Paint; verify activation resolves in-mode"),
    "brush.scale_size": (1, "scales brush/unified size"),
    "brush.stencil_control": (1, "moves the brush texture stencil; engine reads the same texture slot"),
    "paint.brush_colors_flip": (1, "flips brush color/secondary; engine COLOR kernel reads brush color"),
    "palette.new": (1, "data-block management"),
    "texture.new": (1, "data-block management"),
    "wm.radial_control": (1, "already bound (F / Shift-F)"),
    "wm.call_menu_pie": (1, "needs the Phase C addon pies as targets"),
    "wm.call_panel": (1, "needs the Phase C context-menu panel as target"),
    "wm.call_asset_shelf_popover": (1, "needs the Phase E asset shelf as target"),
    "wm.context_toggle": (1, "RNA path on the shared brush (smooth stroke)"),
    "wm.context_menu_enum": (1, "RNA path on the shared brush (stroke method)"),
    "object.mode_set": (1, "header mode switch; custom-mode infra handles it"),
    "view3d.toggle_xray": (1, "viewport display toggle"),
    # -- 2: addon operator over existing engine capability --
    "sculpt.brush_stroke": (2, "done: sculptcore.brush_stroke (INVERT/SMOOTH/MASK toggles; Shift-LMB "
                               "switches kernel where vanilla switches brush asset)"),
    "paint.mask_flood_fill": (2, "done: sculptcore.mask_flood_fill (attr-snapshot undo step)"),
    "sculpt.mask_filter": (2, "done: sculptcore.mask_filter (all six types; numpy neighbor math, "
                              "smooth/sharpen/contrast approximate vanilla's exact curves)"),
    "paint.mask_box_gesture": (2, "done: sculptcore.mask_box_gesture (gestures.py modal + exec path; "
                                  "no symmetry passes / front-faces-only option)"),
    "paint.mask_lasso_gesture": (2, "done: sculptcore.mask_lasso_gesture (even-odd polygon test)"),
    "paint.mask_line_gesture": (2, "gestures.py base exists; line variant not built yet"),
    "paint.mask_polyline_gesture": (2, "gestures.py base exists; polyline variant not built yet"),
    "sculpt.mask_from_cavity": (2, "moderate: bake the existing cavity-automask term into the mask"),
    "sculpt.mask_by_color": (2, "moderate: engine color attr exists"),
    "sculpt.mask_from_boundary": (2, "moderate: engine has boundary info"),
    "sculpt.face_sets_create": (2, "done for MASKED: sculptcore.face_sets_create; from-visible needs engine visibility"),
    "sculpt.face_set_edit": (2, "done for GROW/SHRINK: sculptcore.face_set_edit (cursor pick via engine "
                                "raycast, highest-set fallback; engine-face-index pick may mismap after "
                                "dyntopo gaps); FAIR_* deferred"),
    "sculpt.face_sets_randomize_colors": (2, "easy if draw provider colors by seed; else display-only gap"),
    "sculpt.face_set_change_visibility": (3, "needs engine visibility support (none yet)"),
    "object.subdivision_set": (2, "done: sculptcore.subdivision_set (sculpt_levels; no ensure_modifier)"),
    "sculpt.dynamic_topology_toggle": (2, "done: scene.sculptcore_dyntopo prop toggle in the Sculpt menu"),
    "sculpt.dyntopo_detail_size_edit": (2, "done: sculptcore.dyntopo_detail_size_edit radial-edits the active "
                                           "Blender detail prop by detail_type_method (vanilla gizmo optional)"),
    "sculpt.detail_flood_fill": (3, "uniform remesh-to-detail not exposed by the engine; revisit"),
    # -- 3: deferred (engine lacks the feature, or out of scope) --
    "paint.hide_show": (3, "needs engine visibility attribute + draw support"),
    "paint.hide_show_all": (3, "needs engine visibility"),
    "paint.hide_show_masked": (3, "needs engine visibility"),
    "paint.hide_show_lasso_gesture": (3, "needs engine visibility"),
    "paint.hide_show_line_gesture": (3, "needs engine visibility"),
    "paint.hide_show_polyline_gesture": (3, "needs engine visibility"),
    "paint.visibility_filter": (3, "needs engine visibility"),
    "paint.visibility_invert": (3, "needs engine visibility"),
    "sculpt.expand": (3, "modal geodesic expand framework; large"),
    "sculpt.mesh_filter": (3, "sculpt filter framework not in engine"),
    "sculpt.color_filter": (3, "sculpt filter framework"),
    "sculpt.optimize": (3, "SculptSession-specific; no engine meaning (permanent N/A)"),
    "object.voxel_remesh": (3, "voxel remesh not in engine"),
    "object.voxel_size_edit": (3, "voxel remesh not in engine"),
    "sculpt.sample_detail_size": (3, "samples Blender dyntopo/voxel detail; revisit with engine dyntopo UX"),
    "sculpt.trim_box_gesture": (3, "boolean trim not in engine"),
    "sculpt.trim_lasso_gesture": (3, "boolean trim not in engine"),
    "sculpt.trim_line_gesture": (3, "boolean trim not in engine"),
    "sculpt.trim_polyline_gesture": (3, "boolean trim not in engine"),
    "sculpt.project_line_gesture": (3, "line projection (gesture deform) not in engine"),
    "sculpt.paint_mask_extract": (3, "mesh extraction; possible later via flush + mesh ops"),
    "sculpt.paint_mask_slice": (3, "mesh extraction"),
    "sculpt.face_set_extract": (3, "mesh extraction"),
    "sculpt.face_sets_init": (3, "topology/material analysis modes; simple ones may move to 2 later"),
    "sculpt.symmetrize": (3, "topology symmetrize not in engine (stroke mirroring is, see symmetry.py)"),
    "sculpt.set_pivot_position": (3, "tied to sculpt transform support"),
    "transform.translate": (3, "sculpt transform (pivot) path not in engine"),
    "transform.rotate": (3, "sculpt transform path"),
    "transform.resize": (3, "sculpt transform path"),
    "object.transfer_mode": (3, "alt-Q object switch; needs custom-mode support in the C op"),
    "paint.sample_color": (3, "color-paint sampling; revisit with COLOR-brush UX"),
    "curves.convert_to_particle_system": (3, "sculpt-curves menu; different object type, out of scope"),
    "curves.snap_curves_to_surface": (3, "sculpt-curves; out of scope"),
}

# Vanilla chords deliberately left unbound even though the operator itself
# is class-1 (wm.call_menu_pie): the pie's slots are engine gaps. Keys match
# the T6 gate's chord format.
CHORD_ALLOWLIST = {
    "alt+A:PRESS": "automasking pie withheld: only cavity is mapped",
    "alt+W:PRESS": "face-sets pie withheld: 3 of 4 slots are visibility ops",
}

CLASS_TITLES = {
    1: "Class 1 — works as-is (verify in-mode)",
    2: "Class 2 — needs a sculptcore.* engine operator",
    3: "Class 3 — deferred (allowlisted)",
}


def collect():
    tmp = tempfile.gettempdir()
    intro = json.load(open(os.path.join(tmp, "ui_introspect.json")))
    kmd = json.load(open(os.path.join(tmp, "keymap_dump.json")))
    ops = collections.defaultdict(set)

    def walk(node, surface):
        if isinstance(node, dict):
            op = node.get("operator")
            if isinstance(op, str):
                m = re.match(r"bpy\.ops\.([a-z0-9_]+\.[a-z0-9_]+)", op)
                if m:
                    ops[m.group(1)].add(surface)
            for value in node.values():
                walk(value, surface)
        elif isinstance(node, list):
            for value in node:
                walk(value, surface)

    for key, rec in intro.items():
        if key.startswith("SCULPT/") and rec.get("ok"):
            walk(rec["items"], key.split("/", 1)[1])
    for item in kmd["vanilla"]["items"]:
        ops[item["idname"]].add("KEYMAP")
    return ops


def main():
    ops = collect()
    rows = collections.defaultdict(list)  # class -> rows
    unclassified = []
    for op in sorted(ops):
        surfaces = ", ".join(sorted(ops[op]))
        entry = CLASSIFY.get(op)
        if entry is None:
            unclassified.append((op, surfaces))
        else:
            rows[entry[0]].append((op, surfaces, entry[1]))

    lines = [
        "# P10 Phase 0 — Operator Audit (generated)",
        "",
        "Generated by `claudeMemory/scripts/ui_parity_audit.py` from the T2/T3",
        "dumps; the classification table lives in that script. Do not edit this",
        "file by hand — edit the script and rerun.",
        "",
        "Total operators referenced by the vanilla sculpt UI: {:d}".format(len(ops)),
        "",
    ]
    for cls in (1, 2, 3):
        lines += ["## " + CLASS_TITLES[cls], "",
                  "| Operator | Surfaces | Note |", "|---|---|---|"]
        for op, surfaces, note in rows[cls]:
            lines.append("| `{:s}` | {:s} | {:s} |".format(op, surfaces, note))
        lines.append("")
    if unclassified:
        lines += ["## UNCLASSIFIED (new surface — classify in the script)", ""]
        for op, surfaces in unclassified:
            lines.append("- `{:s}` ({:s})".format(op, surfaces))
        lines.append("")
    with open(AUDIT_MD, "w") as f:
        f.write("\n".join(lines))

    allow = {
        "_comment": "Generated by ui_parity_audit.py; class-3 operators with reasons. "
                    "T6 treats keymap chords / menu entries bound to these as intentional gaps.",
        "operators": {op: note for op, _s, note in rows[3]},
        "chords": CHORD_ALLOWLIST,
    }
    with open(ALLOWLIST, "w") as f:
        json.dump(allow, f, indent=1, sort_keys=True)

    counts = {cls: len(rows[cls]) for cls in (1, 2, 3)}
    print("audit: {:d} ops -> class1 {:d}, class2 {:d}, class3 {:d}, unclassified {:d}".format(
        len(ops), counts[1], counts[2], counts[3], len(unclassified)))
    print("wrote", AUDIT_MD)
    print("wrote", ALLOWLIST)


if __name__ == "__main__":
    main()
