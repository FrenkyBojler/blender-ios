"""UI parity T3: keymap dump + chord-level diff, vanilla Sculpt vs SculptCore.

Serializes the vanilla "Sculpt" keymap (from the user keyconfig — the merged,
actually-active items) and the addon's "SculptCore Mode" keymap (addon
keyconfig), then diffs them by key chord (type/value/modifiers/key-modifier).
Idnames are expected to differ (sculpt.* vs sculptcore.*); the diff reports
what each side binds on a chord, so parity means "every vanilla chord has a
SculptCore binding or a deliberate allowlist entry", not identical idnames.

Also lists (names only) the other keymaps whose name mentions Sculpt — the
per-tool keymaps ("3D View Tool: Sculpt, ...") that become relevant once the
toolbar is ported.

Operator properties are serialized only where explicitly set (matching what
keyconfig exports do), so defaults don't drown the diff.

Run: blender --factory-startup --python keymap_dump.py
  -> %TEMP%/keymap_dump.txt (diff) + %TEMP%/keymap_dump.json (full dump)
GUI launch (e.g. via run_harness.py) is the reliable way to get fully
populated keyconfigs; --background works only if keymaps load there.
"""
import json
import os
import tempfile

import bpy

OUT = os.path.join(tempfile.gettempdir(), "keymap_dump.txt")
OUT_JSON = os.path.join(tempfile.gettempdir(), "keymap_dump.json")

VANILLA_KEYMAP = "Sculpt"
ADDON_KEYMAP = "SculptCore Mode"


def _props_dict(props):
    if props is None:
        return {}
    out = {}
    for prop in props.bl_rna.properties:
        ident = prop.identifier
        if ident == "rna_type" or not props.is_property_set(ident):
            continue
        try:
            value = getattr(props, ident)
        except AttributeError:
            continue
        if prop.type == 'POINTER':
            value = repr(value)
        elif prop.type == 'COLLECTION':
            value = "<collection[{:d}]>".format(len(value))
        elif getattr(prop, "is_array", False):
            value = list(value)
        out[ident] = value
    return out


def _chord(kmi):
    parts = []
    for mod in ("ctrl", "shift", "alt", "oskey"):
        v = getattr(kmi, mod)
        if v == 1:
            parts.append(mod.capitalize())
        elif v == -1:
            parts.append("{:s}?".format(mod.capitalize()))
    if kmi.any:
        parts.append("Any")
    if kmi.key_modifier != 'NONE':
        parts.append(kmi.key_modifier)
    parts.append(kmi.type)
    return "+".join(parts) + " " + kmi.value


def _item(kmi):
    return {
        "chord": _chord(kmi),
        "idname": kmi.idname,
        "active": kmi.active,
        "repeat": kmi.repeat,
        "map_type": kmi.map_type,
        "properties": _props_dict(kmi.properties),
    }


def _dump_keymap(km):
    return {
        "name": km.name,
        "space_type": km.space_type,
        "region_type": km.region_type,
        "is_modal": km.is_modal,
        "items": [_item(kmi) for kmi in km.keymap_items],
    }


def _fmt(item):
    props = "" if not item["properties"] else " {!r}".format(item["properties"])
    off = "" if item["active"] else " (disabled)"
    return "{:s}{:s}{:s}".format(item["idname"], props, off)


def main():
    lines = ["Keymap diff: {!r} (user kc) vs {!r} (addon kc)".format(
        VANILLA_KEYMAP, ADDON_KEYMAP), ""]
    try:
        wm = bpy.context.window_manager
        if "sculptcore_addon" not in bpy.context.preferences.addons:
            bpy.ops.preferences.addon_enable(module="sculptcore_addon")

        vanilla_km = wm.keyconfigs.user.keymaps.get(VANILLA_KEYMAP)
        if vanilla_km is None:
            vanilla_km = wm.keyconfigs.default.keymaps.get(VANILLA_KEYMAP)
        addon_km = wm.keyconfigs.addon.keymaps.get(ADDON_KEYMAP)
        if vanilla_km is None:
            raise RuntimeError(
                "keymap {!r} not found (run in GUI, keyconfigs may not load in "
                "--background)".format(VANILLA_KEYMAP))
        if addon_km is None:
            raise RuntimeError("keymap {!r} not found; addon enabled?".format(ADDON_KEYMAP))

        vanilla = _dump_keymap(vanilla_km)
        ours = _dump_keymap(addon_km)

        related = sorted(
            km.name for kc in (wm.keyconfigs.user, wm.keyconfigs.default)
            for km in kc.keymaps
            if "sculpt" in km.name.lower() and km.name != VANILLA_KEYMAP)

        with open(OUT_JSON, "w") as f:
            json.dump({"vanilla": vanilla, "sculptcore": ours,
                       "related_keymaps": sorted(set(related))}, f, indent=1)

        by_chord_ours = {}
        for item in ours["items"]:
            by_chord_ours.setdefault(item["chord"], []).append(item)
        matched_chords = set()

        missing = []
        covered = []
        for item in vanilla["items"]:
            matches = by_chord_ours.get(item["chord"])
            if matches:
                matched_chords.add(item["chord"])
                covered.append((item, matches))
            else:
                missing.append(item)
        extra = [i for i in ours["items"] if i["chord"] not in matched_chords]

        lines.append("vanilla items: {:d}   sculptcore items: {:d}   "
                     "covered chords: {:d}   missing: {:d}   extra: {:d}".format(
                         len(vanilla["items"]), len(ours["items"]),
                         len(covered), len(missing), len(extra)))
        lines.append("")
        lines.append("== Vanilla chords with no SculptCore binding ==")
        for item in missing:
            lines.append("  {:<28s} {:s}".format(item["chord"], _fmt(item)))
        lines.append("")
        lines.append("== Covered chords (side by side) ==")
        for item, matches in covered:
            lines.append("  {:<28s} {:s}".format(item["chord"], _fmt(item)))
            for m in matches:
                lines.append("  {:<28s}   -> {:s}".format("", _fmt(m)))
        if extra:
            lines.append("")
            lines.append("== SculptCore chords not in vanilla ==")
            for item in extra:
                lines.append("  {:<28s} {:s}".format(item["chord"], _fmt(item)))
        lines.append("")
        lines.append("related keymaps (names only): {!r}".format(sorted(set(related))))
        lines.append("full dump: {:s}".format(OUT_JSON))
    except Exception as ex:
        import traceback
        lines.append("ERROR: {!r}".format(ex))
        lines.append(traceback.format_exc())
    with open(OUT, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    if not bpy.app.background:
        bpy.ops.wm.quit_blender()


if bpy.app.background:
    main()
else:
    bpy.app.timers.register(lambda: (main(), None)[1], first_interval=1.0)
