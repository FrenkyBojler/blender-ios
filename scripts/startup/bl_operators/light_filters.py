import bpy

# === ENUM DE BASE ===
my_enum_items = [
    ('GOBO', "Gobo", "Utiliser une texture Gobo"),
    ('IES', "IES", "Utiliser un profil IES"),
]

def get_mix_blend_types(self, context):
    from bpy.types import ShaderNodeMixRGB
    return ShaderNodeMixRGB.bl_rna.properties['blend_type'].enum_items.keys()

def update_mix_node_factor(item, context):
    group = bpy.data.node_groups.get("Cycles Filters")
    if not group:
        return
    node = group.nodes.get(item.node_id)
    if node and node.type == 'MIX_RGB':
        node.inputs[0].default_value = item.mix_factor

def update_mix_layering(item, context):
    group = bpy.data.node_groups.get("Cycles Filters")
    if not group:
        return
    node = group.nodes.get(item.node_id)
    if node and node.type == 'MIX_RGB':
        node.blend_type = item.layering

def update_image_texture(item, context):
    group = bpy.data.node_groups.get("Cycles Filters")
    if not group:
        return
    frame = group.nodes.get(item.name)
    if not frame:
        return
    tex_nodes = [n for n in group.nodes if n.parent == frame and n.type == 'TEX_IMAGE']
    if tex_nodes:
        tex_nodes[0].image = item.image

def update_ies_texture(item, context):
    group = bpy.data.node_groups.get("Cycles Filters")
    if not group:
        return
    frame = group.nodes.get(item.name)
    if not frame:
        return
    ies_nodes = [n for n in group.nodes if n.parent == frame and n.type == 'TEX_IES']
    if ies_nodes:
        ies_nodes[0].filepath = item.ies_path
        
def update_ies_mode(item, context):
    group = bpy.data.node_groups.get("Cycles Filters")
    if not group:
        return
    frame = group.nodes.get(item.name)
    if not frame:
        return
    ies_nodes = [n for n in group.nodes if n.parent == frame and n.type == 'TEX_IES']
    if ies_nodes:
        ies_nodes[0].mode = item.ies_mode
        
def update_ies_internal(item, context):
    group = bpy.data.node_groups.get("Cycles Filters")
    if not group:
        return
    frame = group.nodes.get(item.name)
    if not frame:
        return
    ies_nodes = [n for n in group.nodes if n.parent == frame and n.type == 'TEX_IES']
    if ies_nodes and item.ies_internal:
        ies_nodes[0].ies_file = item.ies_internal


def update_frame_label(item, context):
    obj = context.object
    if "Cycles Filters" not in bpy.data.node_groups:
        return
    group = bpy.data.node_groups["Cycles Filters"]
    frame = group.nodes.get(item.name)
    if frame and frame.type == 'FRAME':
        frame.label = item.name
        
def update_gobo_image(item, context):
    group = bpy.data.node_groups.get("Cycles Filters")
    if not group:
        return
    frame = group.nodes.get(item.name)
    if not frame:
        return
    image_nodes = [n for n in group.nodes if n.parent == frame and n.type == 'TEX_IMAGE']
    if image_nodes:
        image_nodes[0].image = item.gobo_image


def update_mix_node_mute(item, context):
    group = bpy.data.node_groups.get("Cycles Filters")
    if not group:
        return

    frame = group.nodes.get(item.name)
    if not frame or frame.type != 'FRAME':
        return

    # Mute/unmute tous les enfants du Frame
    for node in group.nodes:
        if node.parent == frame:
            node.mute = not item.active

    # (Optionnel) rendre le Frame visuellement grisé si désactivé
    frame.use_custom_color = True
    if not item.active:
        frame.color = (0.2, 0.2, 0.2)
    else:
        frame.color = (0.2, 0.3, 0.6)
        
def rebuild_mix_chain():
    context_obj = bpy.context.object
    group_name = f"Cycles Filters_{context_obj.name}"
    group = bpy.data.node_groups.get(group_name)
    if not group:
        return

    g_nodes = group.nodes
    g_links = group.links
    group_output = next((n for n in g_nodes if n.type == 'GROUP_OUTPUT'), None)
    if not group_output:
        return

    # Nettoyer tous les liens existants entre MixRGB et le Group Output
    for input_socket in group_output.inputs:
        while input_socket.is_linked:
            g_links.remove(input_socket.links[0])

    # Nettoyer les liens entre les nœuds MixRGB
    for node in g_nodes:
        if node.type == 'MIX_RGB':
            for input_socket in node.inputs:
                while input_socket.is_linked:
                    g_links.remove(input_socket.links[0])

    # Reconstruire les chaînes pour les GOBO et les IES séparément
    # Peu importe le type actuellement sélectionné
    
    # === CHAÎNE GOBO ===
    gobo_items = [item for item in context_obj.filter_nodes if item.filter_type == 'GOBO']
    previous_output = None

    for item in gobo_items:
        mix_node = g_nodes.get(item.node_id)
        frame_node = g_nodes.get(item.name)
        if not mix_node or not frame_node:
            continue

        # Reconnect texture → input B
        tex_node = next((n for n in g_nodes if n.parent == frame_node and n.type == 'TEX_IMAGE'), None)
        if tex_node:
            g_links.new(tex_node.outputs['Color'], mix_node.inputs['Color2'])

        # Connect mix chain
        if previous_output:
            g_links.new(previous_output, mix_node.inputs['Color1'])

        previous_output = mix_node.outputs['Color']

    # Connecter le dernier mix GOBO à l'entrée Color du Group Output
    if previous_output and group_output and len(group_output.inputs) > 0:
        # Rechercher l'input "Color" ou utiliser l'index 0
        color_socket = None
        for i, socket in enumerate(group_output.inputs):
            if socket.name == 'Color':
                color_socket = socket
                break
        if not color_socket and len(group_output.inputs) > 0:
            color_socket = group_output.inputs[0]
        
        if color_socket:
            g_links.new(previous_output, color_socket)

    # === CHAÎNE IES ===
    ies_items = [item for item in context_obj.filter_nodes if item.filter_type == 'IES']
    previous_output = None

    for item in ies_items:
        mix_node = g_nodes.get(item.node_id)
        frame_node = g_nodes.get(item.name)
        if not mix_node or not frame_node:
            continue

        # Reconnect texture → input B
        ies_node = next((n for n in g_nodes if n.parent == frame_node and n.type == 'TEX_IES'), None)
        if ies_node:
            g_links.new(ies_node.outputs['Fac'], mix_node.inputs['Color2'])

        # Connect mix chain
        if previous_output:
            g_links.new(previous_output, mix_node.inputs['Color1'])

        previous_output = mix_node.outputs['Color']

    # Connecter le dernier mix IES à l'entrée Intensity du Group Output
    if previous_output and group_output and len(group_output.inputs) > 1:
        # Rechercher l'input "Intensity" ou utiliser l'index 1
        intensity_socket = None
        for i, socket in enumerate(group_output.inputs):
            if socket.name == 'Intensity':
                intensity_socket = socket
                break
        if not intensity_socket and len(group_output.inputs) > 1:
            intensity_socket = group_output.inputs[1]
        
        if intensity_socket:
            g_links.new(previous_output, intensity_socket)


# === OPÉRATEUR AJOUT ===
class OBJECT_OT_add_filter_data(bpy.types.Operator):
    bl_idname = "object.add_filter_data"
    bl_label = "Ajouter filtre"
    bl_description = "Ajoute dynamiquement un gobo ou un IES dans le NodeGroup 'Cycles Filters'"

    def execute(self, context):
        # Vérifie le type de filtre sélectionné
        filter_type = context.object.my_filter_type

        if filter_type == 'NONE':
            self.report({'INFO'}, "Select a filter type before adding.")
            return {'CANCELLED'}
        
        obj = context.object
        if not obj or obj.type != 'LIGHT':
            self.report({'ERROR'}, "Sélectionnez une lampe")
            return {'CANCELLED'}

        light = obj.data
        if not light.use_nodes:
            light.use_nodes = True

        node_tree = light.node_tree
        nodes = node_tree.nodes
        links = node_tree.links

        # --- NodeGroup "Cycles Filters" ---
        group_name = f"Cycles Filters_{obj.name}"
        if group_name in bpy.data.node_groups:
            group = bpy.data.node_groups[group_name]
            # Définir le color tag du groupe de nœuds
            group.color_tag = 'TEXTURE'  # Utiliser le tag 'TEXTURE' pour le groupe de nœuds

        else:
            # Créer un nouveau groupe de shader
            group = bpy.data.node_groups.new(name=group_name, type='ShaderNodeTree')
            # Définir le color tag du groupe de nœuds
            group.color_tag = 'TEXTURE'  # Utiliser le tag 'TEXTURE' pour le groupe de nœuds

            # Définir les entrées et sorties du groupe
            # Remarque: dans Blender, nous n'ajoutons pas explicitement de sockets,
            # ils seront créés automatiquement lors de la connexion
            
            # Ajouter les nœuds d'entrée et de sortie
            group_input = group.nodes.new('NodeGroupInput')
            group_input.location = (-1000, 0)
            group_output = group.nodes.new('NodeGroupOutput')
            group_output.location = (200, 0)

        # Vérifier si les sockets Color et Intensity existent déjà dans l'interface
        has_color = False
        has_intensity = False
        
        # Vérifier les sockets existants d'une manière compatible avec toutes les versions
        if hasattr(group.interface, 'items_tree'):
            for item in group.interface.items_tree:
                if item.item_type == 'SOCKET':
                    # Vérifier si c'est une sortie en fonction de son in_out
                    # Note: dans les versions récentes, in_out est une propriété de l'item
                    if hasattr(item, 'in_out') and item.in_out == 'OUTPUT':
                        if item.name == 'Color':
                            has_color = True
                        elif item.name == 'Intensity':
                            has_intensity = True
        
        # Ajouter les sockets seulement s'ils n'existent pas
        if not has_color:
            group.interface.new_socket('Color', description='', in_out='OUTPUT', socket_type='NodeSocketColor', parent=None)
        if not has_intensity:
            group.interface.new_socket('Intensity', description='', in_out='OUTPUT', socket_type='NodeSocketFloat', parent=None)

        # Vérifier si le node group est déjà connecté à l'émission
        emission_node = None
        group_node = None
        
        # Trouver ou créer le node d'émission
        for node in nodes:
            if node.type == 'EMISSION':
                emission_node = node
                break
        if not emission_node:
            emission_node = nodes.new('ShaderNodeEmission')
            emission_node.location = (0, 0)

        # Trouver ou créer le node group
        for node in nodes:
            if node.type == 'GROUP' and node.node_tree == group:
                group_node = node
                break
        if not group_node:
            group_node = nodes.new('ShaderNodeGroup') 
            group_node.node_tree = group
            group_node.location = (-200, 0)

        # Connecter le node group à l'émission
        if not emission_node.inputs['Color'].is_linked:
            links.new(group_node.outputs['Color'], emission_node.inputs['Color'])
        if not emission_node.inputs['Strength'].is_linked:
            links.new(group_node.outputs['Intensity'], emission_node.inputs['Strength'])

        # Connecter l'émission au Light Output
        output_node = None
        for node in nodes:
            if node.type == 'OUTPUT_LIGHT':
                output_node = node
                break
        if not output_node:
            output_node = nodes.new('ShaderNodeOutputLight')
            output_node.location = (400, 0)
            
        if not output_node.inputs['Surface'].is_linked:
            links.new(emission_node.outputs['Emission'], output_node.inputs['Surface'])

        # Aligner le nœud de groupe en fonction du nœud d'émission
        group_node.location = (emission_node.location.x - 200, emission_node.location.y)

        g_nodes = group.nodes
        g_links = group.links

        # Filtrer les MixRGB par type de filtre
        mix_nodes = [n for n in g_nodes if 
                     n.type == 'MIX_RGB' and 
                     n.parent and 
                     n.parent.name.startswith(filter_type)]
        
        is_first = len(mix_nodes) == 0
        
        if filter_type == 'GOBO':
            # Compter seulement les Gobo existants
            gobo_index = len([i for i in obj.filter_nodes if i.filter_type == 'GOBO'])
            y_offset = -gobo_index * 500  # espacement vertical suffisant

            # Ajout des nodes de gobo
            tex_coord = g_nodes.new('ShaderNodeTexCoord')
            tex_coord.location = (-800, y_offset)

            mapping = g_nodes.new('ShaderNodeMapping')
            mapping.location = (-600, y_offset)

            image = g_nodes.new('ShaderNodeTexImage')
            image.location = (-400, y_offset)

            mix = g_nodes.new('ShaderNodeMixRGB')
            mix.blend_type = 'MIX'
            mix.inputs[0].default_value = 1.0
            mix.location = (-200, y_offset)

            # Connect TexCoord > Mapping > Image
            g_links.new(tex_coord.outputs['UV'], mapping.inputs['Vector'])
            g_links.new(mapping.outputs['Vector'], image.inputs['Vector'])
            g_links.new(image.outputs['Color'], mix.inputs['Color2'])

            # Si premier : relie à un Group Input + ajoute Group Output
            if is_first:
                group_input = g_nodes.get("Group Input") or g_nodes.new('NodeGroupInput')
                group_input.location = (-1000, 0)
                group_output = g_nodes.get("Group Output") or g_nodes.new('NodeGroupOutput')
                group_output.location = (200, 0)

                # Essayer de connecter directement sans vérifier si le socket existe
                g_links.new(mix.outputs['Color'], group_output.inputs[0])
            else:
                previous_mix = sorted(mix_nodes, key=lambda n: n.location.y)[-1] if mix_nodes else None
                if previous_mix:
                    g_links.new(previous_mix.outputs['Color'], mix.inputs['Color1'])

                # Rediriger l'output du groupe vers le nouveau mix
                group_output = next((n for n in g_nodes if n.type == 'GROUP_OUTPUT'), None)
                if group_output and len(group_output.inputs) > 0:
                    g_links.new(mix.outputs['Color'], group_output.inputs[0])

            # Créer un node Frame et y regrouper les nodes du gobo
            frame = g_nodes.new('NodeFrame')
            frame_name = f"GOBO_{gobo_index + 1}"
            frame.label = frame_name
            frame.name = frame_name
            frame.location = (-850, y_offset + 100)
            frame.use_custom_color = True
            frame.color = (0.2, 0.3, 0.6)

            # Associer les nodes au frame
            tex_coord.parent = frame
            mapping.parent = frame
            image.parent = frame
            mix.parent = frame

            # Ajouter dans la CollectionProperty (UIList)
            new_item = obj.filter_nodes.add()
            new_item.name = frame_name
            new_item.node_id = mix.name
            new_item.mix_factor = mix.inputs[0].default_value
            new_item.filter_type = 'GOBO'  # Définir explicitement le type
            
        elif filter_type == 'IES':
            # Compter seulement les IES existants
            ies_index = len([i for i in obj.filter_nodes if i.filter_type == 'IES'])
            y_offset = -ies_index * 500  # espacement vertical suffisant
            
            # Décalage horizontal pour les IES (les placer à gauche des GOBO)
            x_offset = -1000  # Décalage horizontal plus grand pour une meilleure séparation visuelle

            # Ajout des nodes de IES
            tex_coord = g_nodes.new('ShaderNodeTexCoord')
            tex_coord.location = (-800 + x_offset, y_offset)

            mapping = g_nodes.new('ShaderNodeMapping')
            mapping.location = (-600 + x_offset, y_offset)

            ies_texture = g_nodes.new('ShaderNodeTexIES')
            ies_texture.location = (-400 + x_offset, y_offset)
            ies_texture.mode = 'EXTERNAL'  # Mode par défaut: External

            mix = g_nodes.new('ShaderNodeMixRGB')
            mix.blend_type = 'MIX'
            mix.inputs[0].default_value = 1.0
            mix.location = (-200 + x_offset, y_offset)

            # Connect TexCoord > Mapping > IES
            g_links.new(tex_coord.outputs['UV'], mapping.inputs['Vector'])
            g_links.new(mapping.outputs['Vector'], ies_texture.inputs['Vector'])
            g_links.new(ies_texture.outputs['Fac'], mix.inputs['Color2'])

            # Si premier : relie à un Group Input + ajoute Group Output
            if is_first:
                group_input = g_nodes.get("Group Input") or g_nodes.new('NodeGroupInput')
                group_input.location = (-1000, 0)
                group_output = g_nodes.get("Group Output") or g_nodes.new('NodeGroupOutput')
                group_output.location = (200, 0)

                # Connecter à l'entrée Intensity (qui sera créée automatiquement)
                if len(group_output.inputs) < 2:  # S'assurer qu'il y a au moins 2 entrées
                    # Le socket 0 est pour Color, créer un deuxième socket pour Intensity
                    dummy_node = g_nodes.new('ShaderNodeValue')
                    dummy_node.location = (100, -100)
                    g_links.new(dummy_node.outputs[0], group_output.inputs[0])  # Créer le premier socket s'il n'existe pas
                    g_links.remove(dummy_node.outputs[0].links[0])  # Supprimer ce lien
                    g_nodes.remove(dummy_node)  # Supprimer le nœud temporaire
                
                # Maintenant connecter à l'entrée 1 (qui est le deuxième socket créé)
                if len(group_output.inputs) > 1:
                    g_links.new(mix.outputs['Color'], group_output.inputs[1])
            else:
                previous_mix = sorted(mix_nodes, key=lambda n: n.location.y)[-1] if mix_nodes else None
                if previous_mix:
                    g_links.new(previous_mix.outputs['Color'], mix.inputs['Color1'])

                # Rediriger l'output du groupe vers le nouveau mix
                group_output = next((n for n in g_nodes if n.type == 'GROUP_OUTPUT'), None)
                if group_output and len(group_output.inputs) > 1:
                    g_links.new(mix.outputs['Color'], group_output.inputs[1])  # Intensity

            # Créer un node Frame et y regrouper les nodes du IES
            frame = g_nodes.new('NodeFrame')
            frame_name = f"IES_{ies_index + 1}"
            frame.label = frame_name
            frame.name = frame_name
            frame.location = (-850 + x_offset, y_offset + 100)
            frame.use_custom_color = True
            frame.color = (0.3, 0.2, 0.6)  # Couleur différente pour IES

            # Associer les nodes au frame
            tex_coord.parent = frame
            mapping.parent = frame
            ies_texture.parent = frame
            mix.parent = frame

            # Ajouter dans la CollectionProperty (UIList)
            new_item = obj.filter_nodes.add()
            new_item.name = frame_name
            new_item.node_id = mix.name
            new_item.mix_factor = mix.inputs[0].default_value
            new_item.filter_type = 'IES'  # Définir explicitement le type

        obj.filter_nodes_index = len(obj.filter_nodes) - 1
        
        # Reconstruire la chaîne de nœuds
        rebuild_mix_chain()

        message = "Nouveau gobo ajouté" if filter_type == 'GOBO' else "Nouveau IES ajouté"
        self.report({'INFO'}, message)
        return {'FINISHED'}


# === OPÉRATEUR SUPPRESSION D'ITEM ===
class OBJECT_OT_remove_filter_node(bpy.types.Operator):
    bl_idname = "object.remove_filter_node"
    bl_label = "Supprimer le filtre"

    index: bpy.props.IntProperty()

    def execute(self, context):
        obj = context.object
        group_name = f"Cycles Filters_{obj.name}"
        if group_name not in bpy.data.node_groups:
            self.report({'ERROR'}, "Groupe de nœuds non trouvé")
            return {'CANCELLED'}

        group = bpy.data.node_groups[group_name]
        g_nodes = group.nodes
        g_links = group.links

        # Identifier le nom du mix à supprimer
        item = obj.filter_nodes[self.index]
        mix_node = g_nodes.get(item.node_id)
        frame_node = g_nodes.get(item.name)

        # Supprimer les connexions liées au MixColor
        if mix_node:
            for link in mix_node.outputs[0].links:
                g_links.remove(link)

        # Supprimer les nodes liés (dans le Frame)
        if frame_node and frame_node.type == 'FRAME':
            children = [n for n in g_nodes if n.parent == frame_node]
            for n in children:
                g_nodes.remove(n)
            # Supprimer le frame après avoir supprimé tous ses enfants
            g_nodes.remove(frame_node)

        # Supprimer les Mix restants si pas encadrés
        if mix_node and mix_node.name in g_nodes:
            g_nodes.remove(mix_node)

        # Supprimer l'entrée UIList
        obj.filter_nodes.remove(self.index)
        if obj.filter_nodes_index >= len(obj.filter_nodes):
            obj.filter_nodes_index = max(0, len(obj.filter_nodes) - 1)
        
        # Reconstruire la chaîne de nœuds
        rebuild_mix_chain()

        self.report({'INFO'}, "Filtre supprimé avec succès")
        return {'FINISHED'}

class OBJECT_OT_move_filter_up(bpy.types.Operator):
    bl_idname = "object.move_filter_up"
    bl_label = "Monter le filtre"

    index: bpy.props.IntProperty()

    def execute(self, context):
        filters = context.object.filter_nodes
        idx = self.index
        current_type = context.object.my_filter_type
        
        # Filtrer par type actuel
        items_by_type = [(i, item) for i, item in enumerate(filters) if item.filter_type == current_type]
        if not items_by_type:
            return {'CANCELLED'}
            
        # Trouver l'élément actuel dans la liste filtrée
        current_pos = -1
        for pos, (i, item) in enumerate(items_by_type):
            if i == idx:
                current_pos = pos
                break
                
        if current_pos > 0:
            # Échanger avec l'élément précédent du même type
            prev_idx = items_by_type[current_pos - 1][0]
            filters.move(idx, prev_idx)
            context.object.filter_nodes_index = prev_idx
            
        rebuild_mix_chain()
        return {'FINISHED'}

class OBJECT_OT_move_filter_down(bpy.types.Operator):
    bl_idname = "object.move_filter_down"
    bl_label = "Descendre le filtre"

    index: bpy.props.IntProperty()

    def execute(self, context):
        filters = context.object.filter_nodes
        idx = self.index
        current_type = context.object.my_filter_type
        
        # Filtrer par type actuel
        items_by_type = [(i, item) for i, item in enumerate(filters) if item.filter_type == current_type]
        if not items_by_type:
            return {'CANCELLED'}
            
        # Trouver l'élément actuel dans la liste filtrée
        current_pos = -1
        for pos, (i, item) in enumerate(items_by_type):
            if i == idx:
                current_pos = pos
                break
                
        if current_pos >= 0 and current_pos < len(items_by_type) - 1:
            # Échanger avec l'élément suivant du même type
            next_idx = items_by_type[current_pos + 1][0]
            filters.move(idx, next_idx)
            context.object.filter_nodes_index = next_idx
            
        rebuild_mix_chain()
        return {'FINISHED'}

# === STRUCTURE DES ÉLÉMENTS DE LA LISTE ===
class FilterNodeItem(bpy.types.PropertyGroup):
    name: bpy.props.StringProperty(
        name="Nom du filtre",
        default="Filtre",
        update=lambda self, context: update_frame_label(self, context)
    )

    layering: bpy.props.EnumProperty(
        name="Layering",
        items=lambda self, context: [
            (k.identifier, k.name, k.description) 
            for k in bpy.types.ShaderNodeMixRGB.bl_rna.properties['blend_type'].enum_items
        ],
        default=0,
        update=lambda self, context: update_mix_layering(self, context)
    )


    mix_factor: bpy.props.FloatProperty(
        name="Mix",
        description="Niveau de mélange",
        default=1.0,
        min=0.0,
        max=1.0,
        update=lambda self, context: update_mix_node_factor(self, context)
    )
    
    active: bpy.props.BoolProperty(
        name="Actif",
        default=True,
        update=lambda self, context: update_mix_node_mute(self, context)
    )

    filter_type: bpy.props.EnumProperty(
        name="Type de filtre",
        items=[
            ('GOBO', "Gobo", "Texture Gobo"),
            ('IES', "IES", "Profile IES"),
        ],
        default='GOBO'
    )

    ies_mode: bpy.props.EnumProperty(
        name="Mode",
        items=[
            ('INTERNAL', "Internal", "Internal mode"),
            ('EXTERNAL', "External", "External mode"),
        ],
        default='EXTERNAL',  # Mode par défaut: External
        update=lambda self, ctx: update_ies_mode(self, ctx)
    )

    node_id: bpy.props.StringProperty(name="Mix Node Name", default="")
    image: bpy.props.PointerProperty(type=bpy.types.Image, update=lambda self, ctx: update_image_texture(self, ctx))
    ies_path: bpy.props.StringProperty(
        name="Fichier IES", 
        default="",
        subtype='FILE_PATH',
        update=lambda self, ctx: update_ies_texture(self, ctx)
    )
    
    ies_internal: bpy.props.PointerProperty(
        name="IES Interne",
        type=bpy.types.Text,
        description="Fichier IES interne",
        update=lambda self, ctx: update_ies_internal(self, ctx)
    )

    gobo_image: bpy.props.PointerProperty(
        name="Image",
        type=bpy.types.Image,
        description="Texture du Gobo",
        update=lambda self, ctx: update_gobo_image(self, ctx)
    )



# === UIList personnalisée ===
class OBJECT_UL_filter_nodes(bpy.types.UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index):
        # Ne montrer que les éléments du type actuellement sélectionné
        if item.filter_type == context.object.my_filter_type:
            if self.layout_type in {'DEFAULT', 'COMPACT'}:
                row = layout.row(align=True)
                row.prop(item, "active", text="")
                row.prop(item, "name", text="", emboss=False)
                row.separator()
                op = row.operator("object.remove_filter_node", text="", icon="X")
                op.index = index
    
    def filter_items(self, context, data, propname):
        items = getattr(data, propname)
        filter_type = context.object.my_filter_type
        
        # Filtrer par type actuel
        filtered = []
        for i, item in enumerate(items):
            if item.filter_type == filter_type:
                filtered.append(self.bitflag_filter_item)
            else:
                filtered.append(0)
                
        return filtered, []
            

# === PANNEAU ===
class OBJECT_PT_filters_panel(bpy.types.Panel):
    bl_label = "Filters"
    bl_idname = "OBJECT_PT_filters_panel"
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = 'data'

    @classmethod
    def poll(cls, context):
        return context.scene.render.engine == 'CYCLES'

    def draw(self, context):
        layout = self.layout
        obj = context.object

        # Enum + bouton
        row = layout.row(align=True)
        row.prop(obj, "my_filter_type", text="Type")
        row.separator()
        row.operator("object.add_filter_data", text="", icon='ADD')

        # Titre adapté au type de filtre
        filter_type = obj.my_filter_type

        row = layout.row()
        row.template_list("OBJECT_UL_filter_nodes", "", obj, "filter_nodes", obj, "filter_nodes_index")

        col = row.column(align=True)
        col.operator("object.move_filter_up", text="", icon='TRIA_UP').index = obj.filter_nodes_index
        col.operator("object.move_filter_down", text="", icon='TRIA_DOWN').index = obj.filter_nodes_index
        
        # === Paramètres du filtre sélectionné ===
        if 0 <= obj.filter_nodes_index < len(obj.filter_nodes):
            item = obj.filter_nodes[obj.filter_nodes_index]
            # Afficher uniquement si le type correspond au filtre sélectionné
            if item.filter_type == filter_type:
                layout.label(text=f"Settings:")
                
                # Afficher les paramètres spécifiques au type
                if item.filter_type == 'GOBO':
                    layout.template_ID(item, "gobo_image", new="image.new", open="image.open")
                elif item.filter_type == 'IES':
                    layout.prop(item, "ies_mode", text="Mode")
                    if item.ies_mode == 'EXTERNAL':
                        layout.prop(item, "ies_path", text="Path")
                    else:
                        layout.prop(item, "ies_internal", text="File")
                                
                # Paramètres communs
                row = layout.row(align=True)
                row.prop(item, "layering", text="Mode")
                row.scale_x = 2.5
                row.separator()
                row.prop(item, "mix_factor", text="Mix", slider=True)

                

# === REGISTER ===
classes = [
    OBJECT_OT_add_filter_data,
    OBJECT_OT_remove_filter_node,
    OBJECT_OT_move_filter_up,
    OBJECT_OT_move_filter_down,
    FilterNodeItem,
    OBJECT_UL_filter_nodes,
    OBJECT_PT_filters_panel,
]

def register():
    for cls in classes:
        bpy.utils.register_class(cls)

    bpy.types.Object.my_filter_type = bpy.props.EnumProperty(
        name="Filter Type",
        description="Choix du type de filtre",
        items=my_enum_items,
        default=0,
        update=lambda self, context: rebuild_mix_chain()  # Mettre à jour la chaîne en cas de changement
    )

    bpy.types.Object.filter_nodes = bpy.props.CollectionProperty(type=FilterNodeItem)
    bpy.types.Object.filter_nodes_index = bpy.props.IntProperty()

def unregister():
    del bpy.types.Object.my_filter_type
    del bpy.types.Object.filter_nodes
    del bpy.types.Object.filter_nodes_index

    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)

if __name__ == "__main__":
    register()
