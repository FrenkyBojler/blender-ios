import bpy


# TODO
# - How do I create an action slot
# - How do I get an action slot
# - How do I create keyframes/fcurves
# - How do I assign existing animation data to a animated charater
# - How do I read keyframes/fcurves

# TODO REMOVE ME LATER: This is just to notes
# https://developer.blender.org/docs/release_notes/5.0/python_api/#animation-rigging
# https://developer.blender.org/docs/release_notes/4.4/upgrading/slotted_actions/
# https://developer.blender.org/docs/release_notes/4.4/python_api/#slotted-actions

"""
Creating a New Action and Slot
"""

# Actions creation.
action = bpy.data.actions.new("SuzanneAction")

# Creation of slots requires an ID type and a name.
slot = action.slots.new(id_type='OBJECT', name="Suzanne")
print(f"slot type={slot.target_id_type} name={slot.name_display} identifier={slot.identifier}")
# Output:
#   slot type=OBJECT name=Suzanne identifier=OBSuzanne


"""
Accessing Action Slots
"""
# Get the data-blocks that are animated by a certain slot:
for action in bpy.data.actions:
    print(f"Action: {action.name}")
    for slot in action.slots:
        print(f"  slot {slot.identifier} is ", end="")
        slot_users = slot.users()
        if not slot_users:
            print("unused")
            continue
        print("used by:")
        for datablock in slot_users:
            print(f"    - {datablock!r}")


"""
Creating Keyframes in an Action Slot
"""
# Actions are created as before:
action = bpy.data.actions.new("SuzanneAction")

# Assign the Action so that Blender knows about the relationship with Suzanne:
suzanne = bpy.data.objects["Suzanne"]
suzanne.animation_data_create().action = action

# Create the F-Curves:
loc_x = action.fcurve_ensure_for_datablock(suzanne, "location", index=0)
loc_y = action.fcurve_ensure_for_datablock(suzanne, "location", index=1)
loc_z = action.fcurve_ensure_for_datablock(suzanne, "location", index=2)

# TODO not sure if "suzanne.keyframe_insert("location", index=0)" is preferred?


"""
Creating Keyframes on a Specific F-Curve 
"""
from bpy_extras import anim_utils

# Suzanne is assumed to already be animated:
suzanne = bpy.data.objects["Suzanne"]
action = suzanne.animation_data.action
action_slot = suzanne.animation_data.action_slot
channelbag = anim_utils.action_get_channelbag_for_slot(action, action_slot)

# Now you can access the F-Curves in the channelbag:
for fcurve in channelbag.fcurves:
    print(f"FCurve: {fcurve.data_path}[{fcurve.array_index}]")


"""
Adding Layers to an Action Slot
"""
# F-Curves and Channel Groups are stored on an infinite keyframe strip that sits on a layer.
layer = action.layers.new("Layer")
strip = layer.strips.new(type='KEYFRAME')
channelbag = strip.channelbag(slot, ensure=True)
