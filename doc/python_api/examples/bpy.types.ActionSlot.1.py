import bpy


"""
Creating and Accessing Action Slots
"""
# Assume Suzanne mesh is present in the scene.
suzanne = bpy.data.objects["Suzanne"]

# Create animation data and an action for Suzanne:
# Slot will be automatically created.
suzanne.keyframe_insert("location", index=0)

# Action slots can be accessed like this:
action = suzanne.animation_data.action
for slot in action.slots:
    print(f"Slot Identifier '{slot.identifier}'", end=" ")
    print(f"with name '{slot.name_display}'", end=" ")
    print(f"targets ID type '{slot.target_id_type}'")


"""
Maually Creating Action Slots
"""
# Actions creation.
action = bpy.data.actions.new("SuzanneAction")

# Creation of slots requires an ID type and a name.
slot = action.slots.new(id_type='OBJECT', name="Suzanne")
print(f"slot type={slot.target_id_type} name={slot.name_display} identifier={slot.identifier}")
# Output:
#   slot type=OBJECT name=Suzanne identifier=OBSuzanne


"""
Explicitly Assigning Slots
"""
# If there are multiple slots on the Action, pick the first one that's compatible
anim_data = suzanne.animation_data_create()
anim_data.action = action
anim_data.action_slot = anim_data.action_suitable_slots[0]


"""
Finding Slot Users
"""
# Iterate through all actions in the Blender data.
for action in bpy.data.actions:
    for slot in action.slots:
        # Return the data-blocks that are animated by this slot of this action
        users = slot.users
