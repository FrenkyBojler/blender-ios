# SPDX-FileCopyrightText: 2026 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy


# Attribute entries are stored as (attribute_name, shader_attribute_type).
# Example:
# {
#     "attribute": [("UVMap", 'GEOMETRY')],
#     "instancer": [("instance_color", 'INSTANCER')],
# }
AttributesMap = dict[str, list[tuple[str, str]]]
ObjectList = list[bpy.types.Object]
ObjectKeySet = set[int]
# Used to de-duplicate entries by both the attribute type and name.
# Example: {('GEOMETRY', "color"), ('INSTANCER', "color")}
AttributeKeySet = set[tuple[str, str]]


INSTANCE_POINTCLOUD_INTERNAL_ATTRIBUTES = {
    "position",
    ".reference_index",
    "instance_transform",
}


def _add_attribute_name(
        attributes: AttributesMap,
        used_names: AttributeKeySet,
        key: str,
        name: str,
        attribute_type: str = 'GEOMETRY',
) -> None:
    if not isinstance(name, str) or not name:
        return

    identifier = (attribute_type, name)
    if identifier in used_names:
        return

    if name.startswith("."):
        key = "hidden"

    attributes[key].append((name, attribute_type))
    used_names.add(identifier)


def _object_key(obj: bpy.types.Object) -> int | None:
    try:
        return obj.as_pointer()
    except ReferenceError:
        return None


def _append_object(objects: ObjectList, used_objects: ObjectKeySet, obj: bpy.types.Object | None) -> None:
    if obj is None:
        return

    key = _object_key(obj)
    if key is None or key in used_objects:
        return

    objects.append(obj)
    used_objects.add(key)


def _objects_with_material(material: bpy.types.Material | None) -> ObjectList:
    objects = []
    if material is None:
        return objects

    for obj in bpy.data.objects:
        for slot in getattr(obj, "material_slots", ()):
            if slot.material == material:
                objects.append(obj)
                break

    return objects


def _context_objects(context: bpy.types.Context) -> ObjectList:
    objects = []
    used_objects = set()

    obj = context.object
    _append_object(objects, used_objects, obj)

    material = getattr(obj, "active_material", None)
    for material_obj in _objects_with_material(material):
        _append_object(objects, used_objects, material_obj)

    return objects


def _add_attributes_from_data(attributes: AttributesMap, used_names: AttributeKeySet, data) -> None:
    data_attributes = getattr(data, "attributes", None)
    if not data_attributes:
        return

    for attr in data_attributes:
        name = attr.name
        if name.startswith(".") or not attr.is_internal:
            _add_attribute_name(attributes, used_names, "attribute", name)


def _add_vertex_groups_from_object(
        attributes: AttributesMap,
        used_names: AttributeKeySet,
        obj: bpy.types.Object,
) -> None:
    vertex_groups = getattr(obj, "vertex_groups", None)
    if not vertex_groups:
        return

    for vertex_group in vertex_groups:
        _add_attribute_name(attributes, used_names, "vertex_group", vertex_group.name)


def _modifier_output_attribute_name(modifier: bpy.types.NodesModifier, identifier: str) -> str:
    properties = getattr(modifier, "properties", None)
    outputs = getattr(properties, "outputs", None)
    if outputs is not None:
        output = getattr(outputs, identifier, None)
        name = getattr(output, "attribute_name", "")
        if isinstance(name, str):
            return name

    try:
        return modifier["{:s}_attribute_name".format(identifier)]
    except (KeyError, TypeError):
        return ""


def _attribute_type_from_domain(domain: str) -> str:
    if domain == 'INSTANCE':
        return 'INSTANCER'

    return 'GEOMETRY'


def _add_modifier_attributes_from_object(
        attributes: AttributesMap,
        used_names: AttributeKeySet,
        obj: bpy.types.Object,
) -> None:
    for modifier in getattr(obj, "modifiers", ()):
        if modifier.type != 'NODES' or modifier.node_group is None:
            continue

        for item in modifier.node_group.interface.items_tree:
            if item.item_type != 'SOCKET' or item.in_out != 'OUTPUT':
                continue

            attribute_type = _attribute_type_from_domain(getattr(item, "attribute_domain", ''))
            _add_attribute_name(
                attributes,
                used_names,
                "instancer" if attribute_type == 'INSTANCER' else "modifier",
                _modifier_output_attribute_name(modifier, item.identifier),
                attribute_type,
            )


def _evaluated_object(obj: bpy.types.Object, depsgraph: bpy.types.Depsgraph) -> bpy.types.Object | None:
    try:
        return obj.evaluated_get(depsgraph)
    except (ReferenceError, RuntimeError):
        return None


def _add_evaluated_attributes_from_object(
        attributes: AttributesMap,
        used_names: AttributeKeySet,
        obj: bpy.types.Object,
        depsgraph: bpy.types.Depsgraph,
) -> None:
    eval_obj = _evaluated_object(obj, depsgraph)
    if eval_obj is None:
        return

    _add_attributes_from_data(attributes, used_names, getattr(eval_obj, "data", None))


def _add_instance_attributes_from_object(
        attributes: AttributesMap,
        used_names: AttributeKeySet,
        obj: bpy.types.Object,
        depsgraph: bpy.types.Depsgraph,
) -> None:
    try:
        eval_obj = depsgraph.id_eval_get(obj)
    except (AttributeError, ReferenceError, RuntimeError):
        eval_obj = _evaluated_object(obj, depsgraph)

    if eval_obj is None:
        return

    evaluated_geometry = getattr(eval_obj, "evaluated_geometry", None)
    if evaluated_geometry is None:
        return

    try:
        geometry = evaluated_geometry()
    except (AttributeError, RuntimeError):
        return

    instance_references = getattr(geometry, "instance_references", None)
    if instance_references is None:
        return

    try:
        references = instance_references()
    except RuntimeError:
        return

    for geometry_reference in references:
        for data_name in ("mesh", "curves", "pointcloud", "instances_pointcloud", "grease_pencil"):
            _add_attributes_from_data(attributes, used_names, getattr(geometry_reference, data_name, None))


def _add_instance_point_attributes_from_object(
        attributes: AttributesMap,
        used_names: AttributeKeySet,
        obj: bpy.types.Object,
        depsgraph: bpy.types.Depsgraph,
) -> None:
    eval_obj = _evaluated_object(obj, depsgraph)
    if eval_obj is None:
        return

    evaluated_geometry = getattr(eval_obj, "evaluated_geometry", None)
    if evaluated_geometry is None:
        return

    try:
        geometry = evaluated_geometry()
    except (AttributeError, RuntimeError):
        return

    instances_pointcloud = getattr(geometry, "instances_pointcloud", None)
    if instances_pointcloud is None:
        return

    try:
        pointcloud = instances_pointcloud()
    except RuntimeError:
        return

    pointcloud_attributes = getattr(pointcloud, "attributes", None)
    if not pointcloud_attributes:
        return

    for attr in pointcloud_attributes:
        name = attr.name
        if name in INSTANCE_POINTCLOUD_INTERNAL_ATTRIBUTES:
            continue
        _add_attribute_name(attributes, used_names, "instancer", name, 'INSTANCER')


def object_attribute_names(context: bpy.types.Context) -> AttributesMap:
    attributes = {
        "modifier": [],
        "vertex_group": [],
        "attribute": [],
        "instancer": [],
        "hidden": [],
    }
    used_names = set()
    depsgraph = context.evaluated_depsgraph_get()

    for obj in _context_objects(context):
        _add_modifier_attributes_from_object(attributes, used_names, obj)
        _add_vertex_groups_from_object(attributes, used_names, obj)
        _add_attributes_from_data(attributes, used_names, getattr(obj, "data", None))
        _add_evaluated_attributes_from_object(attributes, used_names, obj, depsgraph)
        _add_instance_attributes_from_object(attributes, used_names, obj, depsgraph)
        _add_instance_point_attributes_from_object(attributes, used_names, obj, depsgraph)

    return attributes
