import bpy
from mathutils import Vector
import json
from pprint import pprint

with open("/home/habib/blender-git/files/nodes-json/save.json", "r") as f:
    node_tree_dict = json.load(f)


def import_nodes(node_tree_dict: dict, node_tree: bpy.types.NodeTree):

    nodes = node_tree_dict["Nodes"]
    links = node_tree_dict["Links"]

    for n in nodes:
        new_node = node_tree.nodes.new(n["bl_idname"])
        new_node.name = n["name"]
        new_node.location = Vector(n["location"])
        if "node_tree" in n.keys():
            new_node_tree = bpy.data.node_groups.new(name=n["node_tree"]["name"], type="CompositorNodeTree")
            for item in n["node_tree"]["interface"]:
                new_node_tree.interface.new_socket(name=item["name"], description="",
                                                   in_out=item["in_out"], socket_type=item["socket_type"])
            new_node.node_tree = new_node_tree

    for l in links:
        node_tree.links.new(node_tree.nodes[l["from_node"]].outputs[l["from_socket"]],
                            node_tree.nodes[l["to_node"]].inputs[l["to_socket"]])

    for n in nodes:
        if "node_tree" in n.keys():
            import_nodes(n["node_tree"], new_node_tree)


print("\n\n######## NEW RUN ########")
node_tree = bpy.data.node_groups["Load tree"]
node_tree.nodes.clear()
import_nodes(node_tree_dict["node_tree"], node_tree)