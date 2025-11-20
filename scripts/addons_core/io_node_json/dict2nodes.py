import bpy
from mathutils import Vector

def dict2nodes(node_tree_dict: dict, node_tree: bpy.types.NodeTree):

    nodes = node_tree_dict["Nodes"]
    links = node_tree_dict["Links"]

    renamed_map = {}

    # XXX Hacky way to extract node options
    # todo(habib): remove
    ref_node = node_tree.nodes.new('ShaderNodeBlackbody')
    for n in nodes:
        new_node = node_tree.nodes.new(n["bl_idname"])
        new_node.location = Vector(n["location"])

        for op in n.keys():
            try:
                setattr(new_node, op, n[op])
            except:
                pass

        # Nodes have unique names. Update node map if the newly created node has a different name.
        # Creating links rely on accurate names
        if new_node.name != n["name"]:
            renamed_map[n["name"]] = new_node.name

        if "node_tree" in n.keys():
            new_node_tree = bpy.data.node_groups.new(name=n["node_tree"]["name"], type="CompositorNodeTree")
            for item in n["node_tree"]["interface"]:
                new_node_tree.interface.new_socket(name=item["name"], description="",
                                                   in_out=item["in_out"], socket_type=item["socket_type"])
            new_node.node_tree = new_node_tree

    for l in links:
        if l["from_node"] in renamed_map:
            l["from_node"] = renamed_map[l["from_node"]]
        if l["to_node"] in renamed_map:
            l["to_node"] = renamed_map[l["to_node"]]

        node_tree.links.new(node_tree.nodes[l["from_node"]].outputs[l["from_socket"]],
                            node_tree.nodes[l["to_node"]].inputs[l["to_socket"]])

    for n in nodes:
        if "node_tree" in n.keys():
            dict2nodes(n["node_tree"], new_node_tree)

    node_tree.nodes.remove(ref_node)

debug = False
if debug:
    import json
    with open("/home/habib/blender-git/files/nodes-json/save.json", "r") as f:
        node_tree_dict = json.load(f)


    print("\n\n######## NEW RUN ########")
    node_tree = bpy.data.node_groups["Load tree"]
    node_tree.nodes.clear()
    dict2nodes(node_tree_dict["node_tree"], node_tree)

    # import json
    # node_tree_dict = {}
    # node_tree = bpy.data.node_groups["Load tree"]

    # import subprocess
    # data = subprocess.check_output(["wl-paste"], text=True)

    # node_tree_dict = json.loads(data)
    # dict2nodes(node_tree_dict["node_tree"], node_tree)