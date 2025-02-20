# Example add-on by Mets with some edits to show both keymap items.

import bpy
from bpy.types import AddonPreferences
from rna_keymap_ui import draw_kmi

bl_info = {
    "name": "Hotkey Example",
    "blender": (3, 0, 0),
    "category": "Animation",
    "version": (1, 0, 0),
    "description": "Example of an add-on that wants to create a user-customizable shortcut."
}


class HotkeyExampleAddonPrefs(AddonPreferences):
    bl_idname = __name__

    def draw(self, context):
        # This draws our hotkey as stored in the ADDON keyconfig, which is not what we want,
        # since that data represents the default state of the keymap.
        # If the user customizes this, they don't have the ability to reset it to default,
        # AND it will not be saved when they restart Blender.

        # To address both those issues, we need to draw the corresponding KeyMapItem in the USER keyconfig,
        # but there's no PyAPI call to get a reference to that one.
        layout = self.layout
        kc, km, kmi = addon_keymaps[0]
        sub_keymaps = []
        indent_level = 0

        wm = context.window_manager
        kc_user = wm.keyconfigs.user
        km_user = kc_user.keymaps.find_match(km)
        kmi_user = km_user.keymap_items.find_match(km, kmi)

        print("Debugging purposes only:")
        print(kmi_user, kmi_user.to_string())
        print(kmi, kmi.to_string())

        layout.label(text="Original (for comparison)")
        draw_kmi(sub_keymaps, kc, km, kmi, layout, indent_level)
        layout.label(text="User (the one we want to edit)")
        draw_kmi(sub_keymaps, kc_user, km_user, kmi_user, layout, indent_level)


addon_keymaps = []


def register_keymap():
    # Standard way that add-ons register and store their keymaps.
    wm = bpy.context.window_manager
    kc = wm.keyconfigs.addon
    if kc:
        km = kc.keymaps.new(name="Screen", space_type="EMPTY")
        kmi = km.keymap_items.new("screen.animation_play", type="W", value="PRESS", ctrl=True, alt=True)
        addon_keymaps.append((kc, km, kmi))


def unregister_keymap():
    for kc, km, kmi in addon_keymaps:
        km.keymap_items.remove(kmi)
    addon_keymaps.clear()


def register():
    bpy.utils.register_class(HotkeyExampleAddonPrefs)
    register_keymap()


def unregister():
    unregister_keymap()
    bpy.utils.unregister_class(HotkeyExampleAddonPrefs)


if __name__ == "__main__":
    register()
