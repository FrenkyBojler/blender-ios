import bpy
view_layer = bpy.context.view_layer

view_layer.lpes.add()
view_layer.active_lpe.name = "Diffuse and Glossy Color"
view_layer.active_lpe.expression = "CDA + CGA"