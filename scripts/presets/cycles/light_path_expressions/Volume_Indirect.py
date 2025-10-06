import bpy
view_layer = bpy.context.view_layer

view_layer.lpes.add()
view_layer.active_lpe.name = "Volume Indirect"
view_layer.active_lpe.expression = "C[DGS]+VL"