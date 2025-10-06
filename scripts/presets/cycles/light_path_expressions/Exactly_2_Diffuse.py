import bpy
view_layer = bpy.context.view_layer

view_layer.lpes.add()
view_layer.active_lpe.name = "Exactly 2 Diffuse"
view_layer.active_lpe.expression = "C.*{D=2}L"