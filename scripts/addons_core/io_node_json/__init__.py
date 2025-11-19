bl_info = {
    "name": "Import/Export nodes as json",
    "author": "Blender Foundation",
    "location": "Node Editor > Node > Import JSON",
    "blender": (5, 1, 0),
    "category": "Node",
}

# todo(habib): reload and other standard addon features

# To support reload properly, try to access a package var,
# if it's there, reload everything
if "bpy" in locals():
    import importlib
    if "dict2nodes" in locals():
        importlib.reload(dict2nodes)

import bpy
from .nodes2dict import nodes2dict
from .dict2nodes import dict2nodes
from bpy_extras.io_utils import ImportHelper

class ImportNodesJson(bpy.types.Operator, ImportHelper):
    """
    todo(habib): doc
    """
    bl_idname = "node.import_json"
    bl_label = "Import nodes from a json file"
    bl_options = {'REGISTER', 'UNDO'}

    filter_glob: bpy.props.StringProperty(
        default="*.*",
        options={'HIDDEN'},
    )

    @classmethod
    def poll(cls, context):
        space = context.space_data
        return (
            (space is not None) and
            space.type == 'NODE_EDITOR' and
            space.node_tree is not None
        )

    def execute(self, context):
        # Open file browser
        # Read Json file as dict
        # Execute dict2nodes

        import json
        print("Selected file:", self.filepath)
        node_tree_dict = {}
        with open(self.filepath, "r") as f:
            node_tree_dict = json.load(f)

        node_tree = context.space_data.node_tree
        dict2nodes(node_tree_dict, node_tree)

        return {'FINISHED'}

def menu_func(self, context):
    self.layout.operator(ImportNodesJson.bl_idname, text="Import JSON")

def register():
    bpy.utils.register_class(ImportNodesJson)
    bpy.types.NODE_MT_node.append(ImportNodesJson)

def unregister():
    bpy.utils.unregister_class(ImportNodesJson)
