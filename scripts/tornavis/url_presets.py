import bpy

def register ():
    # bpy.ops.wm.url_open_preset(type="MBLENDER")

    bpy.types.WM_OT_url_open_preset.preset_items.append(
        (('MBLENDER', "Mechanicalblender.org",  "Mechanical blender's official web-site"),
        "https://www.mechanicalblender.org")
        )
    bpy.types.WM_OT_url_open_preset.preset_items.append(
        (('MBLENDER_DOC', "Mechanicalblender Doc",  "Mechanical blender's documentation"),
        "https://www.mechanicalblender.org/#documentation")
        )

def unregister():
    pass

