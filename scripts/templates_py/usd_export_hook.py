bl_info = {
    "name": "USD Hook Example",
    "blender": (4, 4, 0),
}

import bpy
import bpy.types
import textwrap

# Make `pxr` module available, for running as `bpy` PIP package.
bpy.utils.expose_bundled_modules()

import pxr.Gf as Gf
import pxr.Sdf as Sdf
import pxr.Usd as Usd
import pxr.UsdShade as UsdShade


class USDHookExample(bpy.types.USDHook):
    """Example implementation of USD IO hooks"""
    bl_idname = "usd_hook_example"
    bl_label = "Example"

    @staticmethod
    def on_export(export_context):
        """ Include the Blender filepath in the root layer custom data.
        """

        stage = export_context.get_stage()

        if stage is None:
            return False
        data = bpy.data
        if data is None:
            return False

        # Set the custom data.
        rootLayer = stage.GetRootLayer()
        customData = rootLayer.customLayerData
        customData["blenderFilepath"] = data.filepath
        rootLayer.customLayerData = customData

        return True

    @staticmethod
    def on_material_export(export_context, bl_material, usd_material):
        """ Create a simple MaterialX shader on the exported material.
        """

        stage = export_context.get_stage()

        # Create a MaterialX standard surface shader
        mtl_path = usd_material.GetPrim().GetPath()
        shader = UsdShade.Shader.Define(stage, mtl_path.AppendPath("mtlxstandard_surface"))
        shader.CreateIdAttr("ND_standard_surface_surfaceshader")

        # Connect the shader.  MaterialX materials use "mtlx" renderContext
        usd_material.CreateSurfaceOutput("mtlx").ConnectToSource(shader.ConnectableAPI(), "out")

        # Set the color to the Blender material's viewport display color.
        col = bl_material.diffuse_color
        shader.CreateInput("base_color", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(col[0], col[1], col[2]))

        return True

    @staticmethod
    def on_import(import_context):
        """ Create a text object to display the stage's custom data.
        """
        stage = import_context.get_stage()

        if stage is None:
            return False

        # Get the custom data.
        rootLayer = stage.GetRootLayer()
        customData = rootLayer.customLayerData

        # Create a text object to display the stage path
        # and custom data dictionary entries.

        bpy.ops.object.text_add()
        ob = bpy.context.view_layer.objects.active

        if (ob is None) or (ob.data is None):
            return False

        ob.name = "layer_data"
        ob.data.name = "layer_data"

        # The stage root path is the first line.
        text = rootLayer.realPath

        # Append key/value strings, enforcing text wrapping.
        for item in customData.items():
            print(item)
            text += '\n'
            line = str(item[0]) + ': ' + str(item[1])
            text += textwrap.fill(line, width=80)

        ob.data.body = text

        return True

    @staticmethod
    def material_import_poll(import_context, usd_material):
        """
        Return True if the given USD material can be converted.
        Return False otherwise.
        """
        # We can convert MaterialX.
        surf_output = usd_material.GetSurfaceOutput("mtlx")
        return bool(surf_output)

    @staticmethod
    def on_material_import(import_context, bl_material, usd_material):
        """
        Import a simple mtlx material.  Just handle the base_color input
        of a ND_standard_surface_surfaceshader.
        """

        # We must confirm that we can handle this material.
        surf_output = usd_material.GetSurfaceOutput("mtlx")
        if not surf_output:
            return False

        if not surf_output.HasConnectedSource():
            return False

        # Get the connected surface output source.
        source = surf_output.GetConnectedSource()
        # Get the shader prim from the source
        shader = UsdShade.Shader(source[0])
        shader_id = shader.GetShaderId()
        if shader_id != "ND_standard_surface_surfaceshader":
            return False

        color_attr = shader.GetInput("base_color")
        if color_attr is None:
            return False

        # Create the node tree
        bl_material.use_nodes = True
        node_tree = bl_material.node_tree
        nodes = node_tree.nodes
        bsdf = nodes.get("Principled BSDF")
        assert bsdf
        bsdf_base_color_input = bsdf.inputs['Base Color']

        # Try to set the default color value.
        # Get the authored default value
        color = color_attr.Get()

        if color is None:
            return False

        bsdf_base_color_input.default_value = (color[0], color[1], color[2], 1)

        return True


def register():
    bpy.utils.register_class(USDHookExample)


def unregister():
    bpy.utils.unregister_class(USDHookExample)


if __name__ == "__main__":
    register()
