import bpy

# Print the name of the active object
active_object = bpy.context.active_object
if active_object:
    print(f"Active Object Name: {active_object.name}")

# Print the names and types of all selected objects.
selected_objects = bpy.context.selected_objects
print("Selected Objects:")
for obj in selected_objects:
    print(f" - {obj.name} (Type: {obj.type})")

# Get the current scene and print its frame rate.
print(f"Current Scene Frame Rate: {bpy.context.scene.render.fps}")

# Last selected collection in the outliner
print(f"Current Collection Name: {bpy.context.collection.name}")



# Accessing objects in the scene vs view layer objects
print("Scene Objects:")
for obj in bpy.context.scene.objects:  # All objects in the scene
    print(f" - {obj.name}")

print("View Layer Objects:")
for obj in bpy.context.view_layer.objects:  # Objects linked to active collections
    print(f" - {obj.name}")



# Working with context.mode specific attributes
if bpy.context.mode == 'POSE':
    # Accessing the active pose bone (only available in Pose mode)
    active_pose_bone = bpy.context.active_pose_bone
    if active_pose_bone:
        print(f"Active Pose Bone: {active_pose_bone.name}")

elif bpy.context.mode == 'OBJECT':
    # Accessing the active object (available in Object mode)
    active_object = bpy.context.active_object
    if active_object:
        print(f"Active Object: {active_object.name}")



# Looping through windows, screens, and areas to find a specific area type
for window in bpy.context.window_manager.windows:
    screen = window.screen
    for area in screen.areas:
        if area.type == 'VIEW_3D':
            print("Found the 3D View area!")
