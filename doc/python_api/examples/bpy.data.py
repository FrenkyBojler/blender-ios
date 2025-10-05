import bpy


# Print all objects in the current file
print("All objects in the file:")
for obj in bpy.data.objects:
    print(obj.name)


# Print all scene names in a list
print("All scenes in the file:")
print(list(bpy.data.scenes.keys()))


# Create a new mesh and object, and link it to the first scene
mymesh = bpy.data.meshes.new("myMesh")  # Create Mesh Data block
myobj = bpy.data.objects.new("myObj", mymesh)  # Create Object Data block


# Add geometry to the mesh (vertices and faces)
mymesh.from_pydata(
    [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)],
    edges=[],
    faces=[(0, 1, 2, 3)],
)

# Link the object to the first scene
scene = bpy.data.scenes[0]
scene.collection.objects.link(myobj)
print(f"Linked object '{myobj.name}' to scene '{scene.name}'")


# Remove a specific mesh if it exists
if "Cube" in bpy.data.meshes:
    mesh = bpy.data.meshes["Cube"]
    print(f"Removing mesh: {mesh.name}")
    bpy.data.meshes.remove(mesh)


# Write images into a file next to the blend.
import os
with open(os.path.splitext(bpy.data.filepath)[0] + ".txt", 'w') as fs:
    for image in bpy.data.images:
        fs.write("{:s} {:d} x {:d}\n".format(image.filepath, image.size[0], image.size[1]))
