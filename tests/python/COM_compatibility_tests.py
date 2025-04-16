import subprocess
import os

blender_44 = "/home/habib/blender-git/Archive/blender-4.4.0-linux-x64/blender"
blender_45 = "/home/habib/blender-git/build_linux/bin/blender"


TEST_FILES = ["dilate.blend",
              "node_bokeh_image.blend",
              "node_dilate_distance.blend",
              "node_dilate_feather.blend",
              "node_dilate_step.blend",
              "node_dilate_threshold.blend",
              "node_mask.blend",
              "node_split_first_linked.blend",
              "node_time.blend",
              "node_tonemap_photoreceptor.blend",
              "node_tonemap_simple.blend",
              "node_inpaint.blend"
              ]

##############################################
#
#            Backward compatibility
#
##############################################

print("Testing backward compatibility...")
dir_backward_compat_test = "/home/habib/blender-git/files/backward_compatibility_test/"

for blend_file in TEST_FILES:
    # Do animation and render using 4.4
    start_file = "/home/habib/blender-git/files/backward_compatibility_ref/" + blend_file

    command = [blender_44]
    command.extend(["--background"])
    command.extend([start_file])
    command.extend(['--python', "/home/habib/blender-git/blender/tests/python/COM_compatibility_vary_and_render.py"])

    output = subprocess.check_output(command)


    # Read file with animation and render using 4.5

    ref_file = start_file.replace(".blend", "_compat_test.blend")
    test_img_path = dir_backward_compat_test + os.path.basename(start_file).replace(".blend", "")
    command = [blender_45]
    command.extend(['--background'])
    command.extend([ref_file])
    command.extend(['--render-output', test_img_path])
    command.extend(['-f', '5'])

    output = subprocess.check_output(command)

    # Compare render outputs

    ref_img_path = start_file.replace(".blend", ".exr")
    test_img_path += "0005.exr"
    command = ['oiiotool', '--diff']
    command.extend([ref_img_path, test_img_path])

    test_name = (os.path.basename(start_file)).replace(".blend", "")
    try:
        output = subprocess.check_output(command)
        if output.find(b"PASS") != -1:
            print("\t", test_name, "\t\tSUCCESS")
    except subprocess.CalledProcessError as e:
        print("\t", test_name, "\t\tFAILED", e.output.decode("utf-8"))



##############################################
#
#            Forward compatibility
#
##############################################
print("Testing forward compatibility...")
dir_forward_compat_test = "/home/habib/blender-git/files/forward_compatibility_test/"
for blend_file in TEST_FILES:
    # Do animation and render in 4.5
    start_file = "/home/habib/blender-git/files/forward_compatibility_ref/" + blend_file
    command = [blender_45]
    command.extend(["--background"])
    command.extend([start_file])
    command.extend(['--python', "/home/habib/blender-git/blender/tests/python/COM_compatibility_vary_and_render.py"])

    output = subprocess.check_output(command)

    # Read file in 4.4

    ref_file = start_file.replace(".blend", "_compat_test.blend")
    test_img_path = dir_forward_compat_test + os.path.basename(start_file).replace(".blend", "")
    command = [blender_44]
    command.extend(['--background'])
    command.extend([ref_file])
    command.extend(['--render-output', test_img_path])
    command.extend(['-f', '5'])

    output = subprocess.check_output(command)

    # Compare render outputs

    ref_img_path = start_file.replace(".blend", ".exr")
    test_img_path += "0005.exr"
    command = ['oiiotool', '--diff']
    command.extend([ref_img_path, test_img_path])

    test_name = (os.path.basename(start_file)).replace(".blend", "")
    try:
        output = subprocess.check_output(command)
        if output.find(b"PASS") != -1:
            print("\t", test_name, "\t\tSUCCESS")
    except subprocess.CalledProcessError as e:
        print("\t", test_name, "\t\tFAILED", e.output.decode("utf-8"))


