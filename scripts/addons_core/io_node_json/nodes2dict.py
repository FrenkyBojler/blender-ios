import bpy

def nodes2dict(node_tree: bpy.types.NodeTree, current_map: dict):
    nodes = node_tree.nodes
    links = node_tree.links

    current_map["node_tree"] = {}
    current_map["node_tree"]["Nodes"] = []
    current_map["node_tree"]["Links"] = []

    interface = []
    for item in node_tree.interface.items_tree:
        item_map = {}
        item_map["name"] = item.name
        item_map["type"] = item.item_type
        item_map["bl_socket_idname"] = item.bl_socket_idname
        item_map["in_out"] = item.in_out
        item_map["socket_type"] = item.socket_type
        interface.append(item_map)

    current_map["node_tree"]["interface"] = interface
    current_map["node_tree"]["name"] = node_tree.name

    for n in nodes:
        node_map = {}
        node_map["name"] = n.name
        node_map["bl_idname"] = n.bl_idname
        node_map["location"] = n.location.to_tuple()

        if hasattr(n, "node_tree"):
            nodes2dict(n.node_tree, node_map)
        current_map["node_tree"]["Nodes"].append(node_map)

    for l in links:
       links_map = {}
       links_map["from_node"] = l.from_node.name
       links_map["from_socket"] = l.from_socket.name

       links_map["to_node"] = l.to_node.name
       links_map["to_socket"] = l.to_socket.name

       current_map["node_tree"]["Links"].append(links_map)


debug = False
if debug:
    # import json
    # print("\n\n###### NEW RUN ########")
    # node_tree = bpy.data.node_groups["Compositor Nodes"]
    # ntree_dict = {}
    # nodes2dict(node_tree, ntree_dict)
    # print("\n")
    # import pprint
    # pprint.pprint(ntree_dict)

    # json_write = json.dumps(ntree_dict)
    # with open("/home/habib/blender-git/files/nodes-json/save.json", "w") as f:
    # #with open("/Users/habib/blender-git/files/json_io/save.json", "w") as f:
    #     json.dump(ntree_dict, f, indent=2)

    import json
    node_tree = bpy.data.node_groups["Compositor Nodes"]
    node_tree_dict = {}
    nodes2dict(node_tree, node_tree_dict)

    data = json.dumps(node_tree_dict, indent=2)
    # todo(habib): support every OS, use pyperclip maybe?
    import subprocess
    subprocess.run(["wl-copy"], input=data, text=True)
