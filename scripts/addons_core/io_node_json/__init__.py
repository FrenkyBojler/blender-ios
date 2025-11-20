bl_info = {
    "name": "Import/Export nodes as json",
    "author": "Blender Foundation",
    "location": "Node Editor > Node > Import JSON",
    "blender": (5, 1, 0),
    "category": "Node",
}

# todo(habib): reload and other standard addon features

import bpy
from .nodes2dict import nodes2dict
from .dict2nodes import dict2nodes
from bpy_extras.io_utils import ImportHelper

class ExportNodesJson(bpy.types.Operator, ImportHelper):
    """
    todo(habib): doc
    """
    bl_idname = "node.export_json"
    bl_label = "export nodes to a json file"
    bl_options = {'REGISTER', 'UNDO'}

    filename_ext = ".json"
    filter_glob: bpy.props.StringProperty(default="*.json", options={'HIDDEN'})

    @classmethod
    def poll(cls, context):
        print("Inside Poll function")
        space = context.space_data
        return (
            (space is not None) and
            space.type == 'NODE_EDITOR' and
            space.node_tree is not None
        )

    def execute(self, context):
        import json
        node_tree = context.space_data.node_tree
        node_tree_dict = {}
        nodes2dict(node_tree, node_tree_dict)

        with open(self.filepath, "w") as f:
            json.dump(node_tree_dict, f, indent=2)
        return {'FINISHED'}

class ImportNodesJson(bpy.types.Operator, ImportHelper):
    """
    todo(habib): doc
    """
    bl_idname = "node.import_json"
    bl_label = "Import nodes from a json file"
    bl_options = {'REGISTER', 'UNDO'}

    filename_ext = ".json"
    filter_glob: bpy.props.StringProperty(default="*.json", options={'HIDDEN'})

    @classmethod
    def poll(cls, context):
        print("Inside Poll function")
        space = context.space_data
        return (
            (space is not None) and
            space.type == 'NODE_EDITOR' and
            space.node_tree is not None
        )

    def execute(self, context):
        import json
        print("Selected file:", self.filepath)
        node_tree_dict = {}
        with open(self.filepath, "r") as f:
            node_tree_dict = json.load(f)

        node_tree = context.space_data.node_tree
        dict2nodes(node_tree_dict["node_tree"], node_tree)

        return {'FINISHED'}

class NODE_MT_import_export(bpy.types.Menu):
    bl_label = "Import/Export"

    def draw(self, context):
        layout = self.layout

        layout.operator("node.import_json", text="Import JSON")
        layout.operator("node.export_json", text="Export JSON")

def menu_func(self, context):
    self.layout.menu("NODE_MT_import_export")



class CopyNodesJson(bpy.types.Operator):
    """
    todo(habib): doc
    """
    bl_idname = "node.json_copy"
    bl_label = "Copy nodes to clipboard"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        space = context.space_data
        return (
            (space is not None) and
            space.type == 'NODE_EDITOR' and
            space.node_tree is not None
        )

    def execute(self, context):
        import json
        node_tree = context.space_data.node_tree
        node_tree_dict = {}
        nodes2dict(node_tree, node_tree_dict)

        data = json.dumps(node_tree_dict, indent=2)
        # todo(habib): support every OS, use pyperclip maybe?
        import subprocess
        subprocess.run(["wl-copy"], input=data, text=True)

        return {'FINISHED'}

class PasteNodesJson(bpy.types.Operator):
    """
    todo(habib): doc
    """
    bl_idname = "node.json_paste"
    bl_label = "Paste nodes from clipboard"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        space = context.space_data
        return (
            (space is not None) and
            space.type == 'NODE_EDITOR' and
            space.node_tree is not None
        )

    def execute(self, context):
        import json
        node_tree_dict = {}
        node_tree = context.space_data.node_tree

        import subprocess
        data = subprocess.check_output(["wl-paste"], text=True)

        node_tree_dict = json.loads(data)
        dict2nodes(node_tree_dict["node_tree"], node_tree)

        return {'FINISHED'}

def register():
    # Import/Export
    bpy.utils.register_class(ImportNodesJson)
    bpy.utils.register_class(ExportNodesJson)

    bpy.utils.register_class(NODE_MT_import_export)
    bpy.types.NODE_MT_node.append(menu_func)

    # Copy/Paste
    bpy.utils.register_class(CopyNodesJson)
    bpy.utils.register_class(PasteNodesJson)


def unregister():
    bpy.utils.unregister_class(ImportNodesJson)
    bpy.utils.unregister_class(ExportNodesJson)

    bpy.utils.unregister_class(CopyNodesJson)
    bpy.utils.unregister_class(PasteNodesJson)

