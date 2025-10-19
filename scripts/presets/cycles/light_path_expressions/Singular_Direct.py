import bpy
view_layer = bpy.context.view_layer

view_layer.lpes.add()
view_layer.active_lpe.name = "Singular Direct"
view_layer.active_lpe.expression = "CSL"
