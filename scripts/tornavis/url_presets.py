import bpy

def register ():
    # bpy.ops.wm.url_open_preset(type="TORNAVIS")

    bpy.types.WM_OT_url_open_preset.preset_items.append(
        (('TORNAVIS', "Tornavis.org",  "Tornavis project official web-site"),
        "https://www.tornavis.org")
        )
    bpy.types.WM_OT_url_open_preset.preset_items.append(
        (('TORNAVIS_DOC', "Tornavis Doc",  "Tornavis project documentation"),
        "https://www.tornavis.org/#documentation")
        )

def unregister():
    pass

