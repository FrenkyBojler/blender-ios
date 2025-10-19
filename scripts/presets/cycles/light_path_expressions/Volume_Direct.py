import bpy
view_layer = bpy.context.view_layer

view_layer.lpes.add()
view_layer.active_lpe.name = "Volume Direct"
view_layer.active_lpe.expression = "CVL"
