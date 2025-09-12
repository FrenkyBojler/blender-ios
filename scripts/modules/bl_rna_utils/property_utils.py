# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-3.0-or-later

import bpy
from typing import Union, Iterator
from bpy.types import ID, PoseBone, Bone, PropertyGroup, Strip, bpy_prop_collection, Object
from bpy.utils import flip_name

"""
Functions to manage runtime properties. Nomenclature and hierarchy:
Runtime Properties - All properties not shipped with core Blender.
    System Properties - Registered by active add-ons. Available on all instances of an Owner type.
        Un-registered System Properties - Registered by add-ons which are now inactive/not installed. Useful to access for versioning.
    Custom Properties - Created via UI by end user.

These functions aim to clarify and/or abstract away these distinctions for common operations like copying/removal.
"""

Owner = Union[ID, PoseBone, Bone, PropertyGroup, Strip]

### Checking functions.

def is_system_prop(owner: Owner, prop_name: str) -> bool:
    """Returns True if a property with the given name was registered by an add-on on owner, 
    even if the add-on is no longer active.
    """
    return prop_name in owner.bl_system_properties_get().keys()

def is_registered_system_prop(owner: Owner, prop_name: str) -> bool:
    """Returns True if a property with the given name is registered by an active add-on."""
    return is_system_prop(owner, prop_name) and prop_name in owner.bl_rna.properties

def is_custom_prop(owner: Owner, prop_name: str) -> bool:
    """Returns True if a property with the given name is present on the owner 
    as a custom property, and not a system property.
    """
    return prop_name in owner.keys() and not is_system_prop(owner, prop_name)

### Functions to get lists of certain types of property names.

def get_all_runtime_prop_names(owner: Owner) -> list[str]:
    return get_custom_prop_names(owner) + get_system_prop_names(owner)

def get_custom_prop_names(owner: Owner) -> Iterator[str]:
    return list(owner.keys())

def get_system_prop_names(owner: Owner) -> list[str]:
    return list(owner.bl_system_properties_get().keys())

### Copy/Mirror functions.

def copy_all_runtime_properties(src: Owner, tgt: Owner, x_mirror=False):
    """Copy add-on and custom properties from source to target. 
    Both should be the same type.
    x_mirror: Flip left/right-sidedness of strings and Object pointers, if found.
    """
    for prop_name in get_all_runtime_prop_names(src):
        copy_runtime_property(src, tgt, prop_name, x_mirror)

def copy_runtime_property(src: Owner, tgt: Owner, prop_name: str, x_mirror=False):
    """Copy add-on properties or custom properties.
    x_mirror: Flip left/right-sidedness of strings and Object pointers, if found.
    """
    # NOTE: This function may be removed in favor of https://projects.blender.org/blender/blender/issues/146099
    if is_system_prop(src, prop_name):
        if is_registered_system_prop(src, prop_name):
            src_prop = getattr(src, prop_name)
            tgt_prop = getattr(tgt, prop_name)
            if isinstance(src_prop, bpy_prop_collection):
                copy_coll_prop(src_prop, tgt_prop, x_mirror)
            elif isinstance(src_prop, PropertyGroup):
                copy_property_group(src_prop, tgt_prop, x_mirror)
            else:
                copy_single_system_prop(src, tgt, prop_name, x_mirror)
        else:
            # HACK: If we need to copy add-on properties, but the add-on is not present, 
            # we have to write to the system properties, which is API abuse that could
            # lose support any moment, but there is no other way to do this atm.
            # tgt_id.bl_system_properties_get()[prop_name] = src_id.bl_system_properties_get()[prop_name]
            tgt_props = tgt.bl_system_properties_get()
            src_props = src.bl_system_properties_get()
            # Remove the existing property before assignment to avoid TypeError when copying IDPropertyGroup
            if prop_name in tgt_props:
                del tgt_props[prop_name]
            tgt_props[prop_name] = src_props[prop_name]
    elif is_custom_prop(src, prop_name):
        copy_custom_property(src, tgt, prop_name)
    else:
        raise Exception(f'{src} has no runtime property called "{prop_name}".')

def copy_property_group(
        src_propgroup: PropertyGroup,
        tgt_propgroup: PropertyGroup,
        x_mirror=False
    ):
    """
    Copy the values from one PropertyGroup into another of the same type.
    x_mirror: Flip left/right-sidedness of strings and Object pointers, if found.
    """
    assert isinstance(tgt_propgroup, PropertyGroup) and isinstance(src_propgroup, PropertyGroup), "Source and target must be PropertyGroups."
    assert tgt_propgroup.__class__ == src_propgroup.__class__, "Source and target must be PropertyGroups of the same type."

    for prop_name in src_propgroup.bl_rna.properties.keys():
        if prop_name in ('rna_type', 'bl_rna'):
            continue
        if not src_propgroup.is_property_set(prop_name):
            tgt_propgroup.property_unset(prop_name)
            continue
        value = getattr(src_propgroup, prop_name)
        if isinstance(value, bpy_prop_collection):
            tgt_collprop = getattr(tgt_propgroup, prop_name)
            copy_coll_prop(value, tgt_collprop, x_mirror)
        elif isinstance(value, PropertyGroup):
            copy_property_group(value, getattr(tgt_propgroup, prop_name), x_mirror)
        else:
            copy_single_system_prop(src_propgroup, tgt_propgroup, prop_name, x_mirror)
    for prop_name in src_propgroup.keys():
        if is_custom_prop(src_propgroup, prop_name):
            # PropertyGroups also support custom properties.
            copy_custom_property(src_propgroup, tgt_propgroup, prop_name, x_mirror)

def copy_coll_prop(
        src_collprop: bpy_prop_collection,
        tgt_collprop: bpy_prop_collection,
        x_mirror=False
    ):
    """
    Copy the values from one CollectionProperty into another of the same type.
    x_mirror: Flip left/right-sidedness of strings and Object pointers, if found.
    """
    assert isinstance(src_collprop, bpy_prop_collection) and isinstance(tgt_collprop, bpy_prop_collection), "Source and target must be CollectionProperties."
    # NOTE: Not sure how to make sure that they are CollectionProperties of the same type.

    tgt_collprop.clear()
    for src_pg in src_collprop:
        assert isinstance(src_pg, PropertyGroup)
        tgt_pg = tgt_collprop.add()
        copy_property_group(src_pg, tgt_pg, x_mirror)

def copy_custom_property(src: Owner, tgt: Owner, prop_name: str, new_name="", x_mirror=False) -> "IDPropertyUIManager":
    """Copy a custom property (one that was created via the UI or via Python dictionary syntax)."""
    if not new_name:
        new_name = prop_name
    src_prop = src.id_properties_ui(prop_name)
    assert src_prop, f'Property "{prop_name}" not found in {src}.'
    value = src[prop_name]
    if x_mirror:
        value = x_mirror_value(value)

    tgt[new_name] = value
    new_prop = tgt.id_properties_ui(new_name)
    new_prop.update_from(src_prop)
    tgt.property_overridable_library_set(f'["{new_name}"]', src.is_property_overridable_library(f'["{prop_name}"]'))
    return tgt.id_properties_ui(new_name)

def copy_single_system_prop(src: Owner, tgt: Owner, prop_name: str, x_mirror=False) -> bool:
    """Attempt to copy a system property from one owner to another, returning success state.
    Will return False for read-only properties, as they cannot be copied.
    Will raise an exception if the property is a PropertyGroup or CollectionProperty.
    """
    if src.is_property_readonly(prop_name):
        # This "early" exit has to come after CollectionProperty & PropertyGroup
        # checks, since they are technically read-only.
        return False

    value = getattr(src, prop_name)
    if type(value) in (PropertyGroup, bpy_prop_collection):
        raise Exception(f'Property "{prop_name}" of {src} is not a single property.')
    if x_mirror:
        value = x_mirror_value(value)
    
    setattr(tgt, prop_name, value)
    return True

### Remove/Rename functions.

def rename_custom_prop(owner, from_name, to_name):
    assert is_custom_prop(owner, from_name), f"Property {from_name} of {owner} is not a Custom Property."
    copy_custom_property(owner, owner, from_name, new_name=to_name, x_mirror=False)
    remove_property(owner, from_name)

def remove_property(obj, prop_name):
    if is_custom_prop(obj, prop_name):
        del obj[prop_name]
    elif is_registered_system_prop(obj, prop_name):
        obj.property_unset(prop_name)
    elif is_system_prop(obj, prop_name):
        disabled_addon_props = obj.bl_system_properties_get()
        del disabled_addon_props[prop_name]
    else:
        raise KeyError(f"{prop_name} not found in {obj.name}")

### X-mirror helper functions.

def x_mirror_value(value: str | Object):
    if isinstance(value, str):
        return flip_name(value)
    elif isinstance(value, Object):
        get_opposite_obj(value)
    else:
        return value

def get_opposite_obj(obj: Object) -> Object:
    """Return the X-mirrored version of a Blender object by name (and library if linked)."""
    flipped_name = flip_name(obj.name)
    lib = obj.library
    return (
        bpy.data.objects.get((lib, flipped_name)) if lib else
        bpy.data.objects.get(flipped_name)
    ) or obj
