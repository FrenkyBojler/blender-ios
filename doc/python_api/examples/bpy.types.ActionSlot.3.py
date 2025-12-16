"""
Explicitly Assigning Action Slots
+++++++++++++++++++++++++++++++++
If there are multiple slots on the Action, and you want to just pick the first one that's
compatible, use the following code. ``anim_data.action_suitable_slots`` can be used `after` the
Action has been assigned; it is a list of action slots of that Action, but only the ones that
are actually compatible with the owner of anim_data (in this case, Suzanne).

"""
# If there are multiple slots on the Action, pick the first one that's compatible
anim_data = suzanne.animation_data_create()
anim_data.action = action
assert anim_data.action_suitable_slots, "expecting at least one suitable slot"
anim_data.action_slot = anim_data.action_suitable_slots[0]


"""
Finding Slot Users
"""
# Iterate through all actions in the Blender data.
print("Action & slot users:")
for action in bpy.data.actions:
    for slot in action.slots:
        # Return the data-blocks that are animated by this slot of this action
        users = slot.users()
        print(f"{action.name:20} slot={slot.identifier:12s} users: {users}")
