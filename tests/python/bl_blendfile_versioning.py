# SPDX-FileCopyrightText: 2023 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# ./blender.bin --background --python tests/python/bl_blendfile_versioning.py ..

# WARNING(@ideasman42): some blend files causes the tests to fail (seemingly) at random (on Linux & macOS at least).
# Take care when adding new files as they may break on other platforms, frequently but not on every execution.
#
# This needs to be investigated!

__all__ = (
    "main",
)

import os
import platform
import sys

import bpy

sys.path.append(os.path.dirname(os.path.realpath(__file__)))
from bl_blendfile_utils import TestHelper


class TestBlendFileOpenLinkSaveAllTestFiles(TestHelper):

    def __init__(self, args):
        self.args = args
        # Some files are known broken currently for opening or linking.
        # They cannot be opened, or will generate some error (e.g. memleaks).
        # Each file in this list should either be the source of a bug report,
        # or removed from tests repo.
        self.excluded_open_link_paths = {
            # modifier_stack/explode_modifier.blend
            # BLI_assert failed: source/blender/blenlib/BLI_ordered_edge.hh:41, operator==(), at 'e1.v_low < e1.v_high'
            "explode_modifier.blend",

            # depsgraph/deg_anim_camera_dof_driving_material.blend
            # ERROR (bke.fcurve):
            # source/blender/blenkernel/intern/fcurve_driver.cc:188 dtar_get_prop_val:
            # Driver Evaluation Error: cannot resolve target for OBCamera ->
            # data.dof_distance
            "deg_anim_camera_dof_driving_material.blend",

            # depsgraph/deg_driver_shapekey_same_datablock.blend
            # Error: Not freed memory blocks: 4, total unfreed memory 0.000427 MB
            "deg_driver_shapekey_same_datablock.blend",

            # physics/fluidsim.blend
            # Error: Not freed memory blocks: 3, total unfreed memory 0.003548 MB
            "fluidsim.blend",

            # opengl/ram_glsl.blend
            # Error: Not freed memory blocks: 4, total unfreed memory 0.000427 MB
            "ram_glsl.blend",
        }

        # Directories to exclude relative to `./tests/files/`.
        self.excluded_open_link_dirs = ()

        # Some files are known broken currently on re-saving & re-opening.
        # Each file in this list should either be the source of a bug report,
        # or removed from tests repo.
        self.excluded_save_reload_paths = {
            # gameengine_bullet_softbody/softbody_constraints.blend
            # Error: on save,
            #        'Unable to pack file, source path '.../gameengine_bullet_softbody/marble_256.jpg' not found'
            "softbody_constraints.blend",

            # gameengine_logic/actuators/sound.blend
            # Error: on save and/or reload, creates memleaks.
            "sound.blend",

            # gameengine_logic/sensors/radar1.blend
            # Error: on save and/or reload, creates memleaks.
            "radar1.blend",

            # io_tests/blend_geometry/edges.blend
            # Error: on save and/or reload, creates memleaks.
            "edges.blend",

            # modeling/geometry_nodes/curve_primitives/bezier_segment.blend
            # Error: on save and/or reload, creates memleaks.
            "bezier_segment.blend",

            # modeling/geometry_nodes/curves/interpolate_curves/group_mismatch.blend
            # Error: on save and/or reload, creates memleaks.
            "group_mismatch.blend",

            # modeling/geometry_nodes/curves/fillet_bezier_and_cyclic.blend
            # Error: on save and/or reload, creates memleaks.
            "fillet_bezier_and_cyclic.blend",

            # modeling/geometry_nodes/curves/interpolate_curves/same_root_position.blend
            # Error: on save and/or reload, creates memleaks.
            "same_root_position.blend",

            # modeling/geometry_nodes/curves/curve_to_points_spiral.blend
            # Error: on save and/or reload, creates memleaks.
            "curve_to_points_spiral.blend",

            # modeling/geometry_nodes/curves/fillet_three_point.blend
            # Error: on save and/or reload, creates memleaks.
            "fillet_three_point.blend",

            # modeling/geometry_nodes/curves/deform_curves_on_surface.blend
            # Error: on save and/or reload, creates memleaks.
            "deform_curves_on_surface.blend",

            # modeling/geometry_nodes/curves/many_nurbs_curves.blend
            # Error: on save and/or reload, creates memleaks.
            "many_nurbs_curves.blend",

            # modeling/geometry_nodes/curves/interpolate_curves/separate_groups.blend
            # Error: on save and/or reload, creates memleaks.
            "separate_groups.blend",

            # modeling/geometry_nodes/curves/mesh_to_curve.blend
            # Error: on save and/or reload, creates memleaks.
            "mesh_to_curve.blend",

            # modeling/geometry_nodes/curves/interpolate_curves/single_group_no_up.blend
            # Error: on save and/or reload, creates memleaks.
            "single_group_no_up.blend",

            # modeling/geometry_nodes/curves/interpolate_curves/single_group_with_surface.blend
            # Error: on save and/or reload, creates memleaks.
            "single_group_with_surface.blend",

            # modeling/geometry_nodes/curves/normals_free_normal_input.blend
            # Error: on save and/or reload, creates memleaks.
            "normals_free_normal_input.blend",

            # modeling/geometry_nodes/curves/interpolate_curves/one_neighbor.blend
            # Error: on save and/or reload, creates memleaks.
            "one_neighbor.blend",

            # modeling/geometry_nodes/curves/knot_structure.blend
            # Error: on save and/or reload, creates memleaks.
            "knot_structure.blend",

            # modeling/geometry_nodes/curves/normals_free_nurbs.blend
            # Error: on save and/or reload, creates memleaks.
            "normals_free_nurbs.blend",

            # modeling/geometry_nodes/curves/primitive_arc.blend
            # Error: on save and/or reload, creates memleaks.
            "primitive_arc.blend",

            # modeling/geometry_nodes/curves/primitive_circle.blend
            # Error: on save and/or reload, creates memleaks.
            "primitive_circle.blend",

            # modeling/geometry_nodes/curves/nurbs_to_bezier.blend
            # Error: on save and/or reload, creates memleaks.
            "nurbs_to_bezier.blend",

            # modeling/geometry_nodes/curves/primitive_quadrilateral.blend
            # Error: on save and/or reload, creates memleaks.
            "primitive_quadrilateral.blend",

            # modeling/geometry_nodes/curves/reverse_bezier_curves.blend
            # Error: on save and/or reload, creates memleaks.
            "reverse_bezier_curves.blend",

            # modeling/geometry_nodes/curves/set_handle_position_topology_change.blend
            # Error: on save and/or reload, creates memleaks.
            "set_handle_position_topology_change.blend",

            # modeling/geometry_nodes/curves/curve_to_points.blend
            # Error: on save and/or reload, creates memleaks.
            "curve_to_points.blend",

            # modeling/geometry_nodes/foreach_geometry_element_zone/stars.blend
            # Error: on save and/or reload, creates memleaks.
            "stars.blend",

            # modeling/geometry_nodes/geometry/duplicate_elements_curve_points.blend
            # Error: on save and/or reload, creates memleaks.
            "duplicate_elements_curve_points.blend",

            # files/libraries_and_linking/library_test_scene.blend
            # Error: on save and/or reload, creates memleaks.
            "library_test_scene.blend",

            # files/libraries_and_linking/libraries/main_scene.blend
            # Error: on save and/or reload, creates memleaks.
            "main_scene.blend",

            # modeling/geometry_nodes/curves/spline_parameter.blend
            # Error: on save and/or reload, creates memleaks.
            "spline_parameter.blend",

            # modeling/geometry_nodes/geometry/realize_instances_two_curves.blend
            # Error: on save and/or reload, creates memleaks.
            "realize_instances_two_curves.blend",

            # modeling/geometry_nodes/curves/string_to_curves_overflow.blend
            # Error: on save and/or reload, creates memleaks.
            "string_to_curves_overflow.blend",

            # modeling/geometry_nodes/curves/curve_topology.blend
            # Error: on save and/or reload, creates memleaks.
            "curve_topology.blend",

            # io_tests/blend_scene/all_objects.blend
            # Error: on save and/or reload, creates memleaks.
            "all_objects.blend",

            # modeling/geometry_nodes/foreach_geometry_element_zone/iteration_domains.blend
            # Error: on save and/or reload, creates memleaks.
            "iteration_domains.blend",

            # modeling/geometry_nodes/curves/set_handle_type.blend
            # Error: on save and/or reload, creates memleaks.
            "set_handle_type.blend",

            # modeling/geometry_nodes/geometry/duplicate_elements_pointcloud_points.blend
            # Error: on save and/or reload, creates memleaks.
            "duplicate_elements_pointcloud_points.blend",

            # modeling/geometry_nodes/foreach_geometry_element_zone/simple_plexus.blend
            # Error: on save and/or reload, creates memleaks.
            "simple_plexus.blend",

            # modeling/geometry_nodes/import/import_obj.blend
            # Error: on reload,
            #        "OBJParser: Cannot read from OBJ file:
            #         '/home/guest/blender/main/build_main_release/tests/blendfile_io/data_files/icosphere.obj'"
            "import_obj.blend",

            # modeling/geometry_nodes/grease_pencil/grease_pencil_curves_conversion.blend
            # Error: on save and/or reload, creates memleaks.
            "grease_pencil_curves_conversion.blend",

            # modeling/geometry_nodes/import/import_csv_dash_separated.blend
            # Error: on save and/or reload, creates memleaks.
            "import_csv_dash_separated.blend",

            # modeling/curve_to_mesh.blend
            # Error: on save and/or reload, creates memleaks.
            "curve_to_mesh.blend",

            # modeling/geometry_nodes/import/import_csv_simple.blend
            # Error: on save and/or reload, creates memleaks.
            "import_csv_simple.blend",

            # modeling/geometry_nodes/curves/string_to_curves_pivot_point.blend
            # Error: on save and/or reload, creates memleaks.
            "string_to_curves_pivot_point.blend",

            # modeling/geometry_nodes/geometry/merge_by_distance_distribute_points.blend
            # Error: on save and/or reload, creates memleaks.
            "merge_by_distance_distribute_points.blend",

            # modeling/geometry_nodes/geometry/duplicate_elements_mesh_points.blend
            # Error: on save and/or reload, creates memleaks.
            "duplicate_elements_mesh_points.blend",

            # modeling/geometry_nodes/import/import_csv_random_floats.blend
            # Error: on save and/or reload, creates memleaks.
            "import_csv_random_floats.blend",

            # modeling/geometry_nodes/mesh/extrude/complex_extrude_and_scale.blend
            # Error: on save and/or reload, creates memleaks.
            "complex_extrude_and_scale.blend",

            # modeling/geometry_nodes/instance/realize_instances_single.blend
            # Error: on save and/or reload, creates memleaks.
            "realize_instances_single.blend",

            # modeling/geometry_nodes/mesh/sample_nearest_surface.blend
            # Error: on save and/or reload, creates memleaks.
            "sample_nearest_surface.blend",

            # modeling/geometry_nodes/mesh/dual_mesh_non_manifold_vert.blend
            # Error: on save and/or reload, creates memleaks.
            "dual_mesh_non_manifold_vert.blend",

            # modeling/geometry_nodes/mesh/triangulate/mixed.blend
            # Error: on save and/or reload, creates memleaks.
            "mixed.blend",

            # modeling/geometry_nodes/mesh/sample_nearest_mesh.blend
            # Error: on save and/or reload, creates memleaks.
            "sample_nearest_mesh.blend",

            # modeling/geometry_nodes/instance/instance_to_points.blend
            # Error: on save and/or reload, creates memleaks.
            "instance_to_points.blend",

            # modeling/geometry_nodes/mesh_primitives/mesh_circle_empty.blend
            # Error: on save and/or reload, creates memleaks.
            "mesh_circle_empty.blend",

            # modeling/geometry_nodes/mesh/sample_uv_surface.blend
            # Error: on save and/or reload, creates memleaks.
            "sample_uv_surface.blend",

            # modeling/geometry_nodes/mesh/extrude/loose_edge_vert_extrude.blend
            # Error: on save and/or reload, creates memleaks.
            "loose_edge_vert_extrude.blend",

            # modeling/geometry_nodes/mesh/extrude/individual_shade_smooth_propagate.blend
            # Error: on save and/or reload, creates memleaks.
            "individual_shade_smooth_propagate.blend",

            # modeling/geometry_nodes/mesh/triangulate/quads_selection.blend
            # Error: on save and/or reload, creates memleaks.
            "quads_selection.blend",

            # modeling/geometry_nodes/points/point_distribute.blend
            # Error: on save and/or reload, creates memleaks.
            "point_distribute.blend",

            # modeling/geometry_nodes/simulation/simple_attribute.blend
            # Error: on save and/or reload, creates memleaks.
            "simple_attribute.blend",

            # modeling/geometry_nodes/mesh/shortest_path.blend
            # Error: on save and/or reload, creates memleaks.
            "shortest_path.blend",

            # modeling/geometry_nodes/mesh/extrude/region_third_edge_type.blend
            # Error: on save and/or reload, creates memleaks.
            "region_third_edge_type.blend",

            # modeling/geometry_nodes/mesh_primitives/mesh_line.blend
            # Error: on save and/or reload, creates memleaks.
            "mesh_line.blend",

            # modeling/geometry_nodes/simulation/index_of_nearest.blend
            # Error: on save and/or reload, creates memleaks.
            "index_of_nearest.blend",

            # modeling/geometry_nodes/simulation/simple_particles.blend
            # Error: on save and/or reload, creates memleaks.
            "simple_particles.blend",

            # modeling/geometry_nodes/utilities/wrong_tree_type.blend
            # Error: on save and/or reload, creates memleaks.
            "wrong_tree_type.blend",

            # modeling/geometry_nodes/points/points_to_curves.blend
            # Error: on save and/or reload, creates memleaks.
            "points_to_curves.blend",

            # modeling/modifiers.blend
            # Error: on save and/or reload, creates memleaks.
            "modifiers.blend",

            # grease_pencil/grease_pencil_paper_pig.blend
            # Error: on save and/or reload, creates memleaks.
            "grease_pencil_paper_pig.blend",

            # modeling/object_conversion.blend
            # Error: on save and/or reload, creates memleaks.
            "object_conversion.blend",

            # modifier_stack/twirl.blend
            # Error: on save and/or reload, creates memleaks.
            "twirl.blend",

            # render/grease_pencil/grease_pencil_motion_blur_center.blend
            # Error: on save and/or reload, creates memleaks.
            "grease_pencil_motion_blur_center.blend",

            # render/grease_pencil/grease_pencil.blend
            # Error: on save and/or reload, creates memleaks.
            "grease_pencil.blend",

            # render/integrator/transparent_shadow_limit_1024.blend
            # Error: on save and/or reload, creates memleaks.
            "transparent_shadow_limit_1024.blend",

            # render/integrator/transparent_shadow_limit_401.blend
            # Error: on save and/or reload, creates memleaks.
            "transparent_shadow_limit_401.blend",

            # render/light/light_ray_visibility.blend
            # Error: on save and/or reload, creates memleaks.
            "light_ray_visibility.blend",

            # render/mesh/double_instance_transformation.blend
            # Error: on save and/or reload, creates memleaks.
            "double_instance_transformation.blend",

            # render/light/light_ray_visibility_light_tree.blend
            # Error: on save and/or reload, creates memleaks.
            "light_ray_visibility_light_tree.blend",

            # render/attributes/attribute_uniform.blend
            # Error: on save and/or reload, creates memleaks.
            "attribute_uniform.blend",

            # render/light/light_tree_multi_distant.blend
            # Error: on save and/or reload, creates memleaks.
            "light_tree_multi_distant.blend",

            # render/grease_pencil/grease_pencil_motion_blur_start.blend
            # Error: on save and/or reload, creates memleaks.
            "grease_pencil_motion_blur_start.blend",

            # render/grease_pencil/grease_pencil_motion_blur_end.blend
            # Error: on save and/or reload, creates memleaks.
            "grease_pencil_motion_blur_end.blend",

            # render/integrator/transparent_shadow_limit_0.blend
            # Error: on save and/or reload, creates memleaks.
            "transparent_shadow_limit_0.blend",

            # modeling/operators.blend
            # Error: on save and/or reload, creates memleaks.
            "operators.blend",

            # render/mesh/normal_types.blend
            # Error: on save and/or reload, creates memleaks.
            "normal_types.blend",

            # render/integrator/transparent_shadow_limit_1.blend
            # Error: on save and/or reload, creates memleaks.
            "transparent_shadow_limit_1.blend",

            # modeling/split_faces_test.blend
            # Error: on save and/or reload, creates memleaks.
            "split_faces_test.blend",

            # render/shader/normal.blend
            # Error: on save and/or reload, creates memleaks.
            "normal.blend",

            # render/mesh/normal_types_motion.blend
            # Error: on save and/or reload, creates memleaks.
            "normal_types_motion.blend",

            # modeling/geometry_nodes/import/import_csv_int_and_float.blend
            # Error: on save and/or reload, creates memleaks.
            "import_csv_int_and_float.blend",

            # render/osl/osl_burley_diffuse.blend
            # Error: on save and/or reload, creates memleaks.
            "osl_burley_diffuse.blend",

            # modeling/geometry_nodes/curves/trim_single_point_last.blend
            # Error: on save and/or reload, creates memleaks.
            "trim_single_point_last.blend",

            # io_tests/blend_geometry/vertices.blend
            # Error: on save and/or reload, creates memleaks.
            "vertices.blend",

            # modeling/geometry_nodes/curves/handle_type_selection.blend
            # Error: on save and/or reload, creates memleaks.
            "handle_type_selection.blend",

            # modeling/geometry_nodes/grease_pencil/sample_index_grease_pencil.blend
            # Error: on save and/or reload, creates memleaks.
            "sample_index_grease_pencil.blend",

            # modeling/geometry_nodes/foreach_geometry_element_zone/multiple_components.blend
            # Error: on save and/or reload, creates memleaks.
            "multiple_components.blend",

            # modeling/geometry_nodes/import/import_ply.blend
            # Error: on reload, 'read_ply_to_mesh: PLY Importer: icosphere: Invalid PLY header.'
            "import_ply.blend",

            # modeling/geometry_nodes/import/import_stl.blend
            # Error: on reload,
            #        'read_stl_file: Failed to open STL file:'...tests/blendfile_io/data_files/icosphere.stl'.'
            "import_stl.blend",

            # render/reports/light_zero_strenght_48790.blend
            # Error: on save and/or reload, creates memleaks.
            "light_zero_strenght_48790.blend",

            # screenshot/viewport/object_modes.blend
            # Error: on save and/or reload, creates memleaks.
            "object_modes.blend",

            # usd/usd_attribute_test.blend
            # Error: on save and/or reload, creates memleaks.
            "usd_attribute_test.blend",
        }

        # Directories to exclude relative to `./tests/files/`.
        self.excluded_save_reload_dirs = ()

        # Some files are expected to be invalid.
        # This mapping stores filenames as keys, and expected error message as value.
        self.invalid_paths = {
            # animation/driver-object-eyes.blend
            # File generated from a big endian build of Blender.
            "driver-object-eyes.blend": (
                (OSError, RuntimeError),
                "created by a Big Endian version of Blender, support for these files has been removed in Blender 5.0"
            ),

            # modeling/faceselectmode.blend
            # File generated from a big endian build of Blender.
            "faceselectmode.blend": (
                (OSError, RuntimeError),
                "created by a Big Endian version of Blender, support for these files has been removed in Blender 5.0"
            ),
            # modeling/weight-paint_test.blend
            # File generated from a big endian build of Blender.
            "weight-paint_test.blend": (
                (OSError, RuntimeError),
                "created by a Big Endian version of Blender, support for these files has been removed in Blender 5.0"
            ),

            # io_tests/blend_big_endian/1.62/glass.blend
            # File generated from a big endian build of Blender.
            "glass.blend": (
                (OSError, RuntimeError),
                "created by a Big Endian version of Blender, support for these files has been removed in Blender 5.0"
            ),
            # io_tests/blend_big_endian/1.62/room.blend
            # File generated from a big endian build of Blender.
            "room.blend": (
                (OSError, RuntimeError),
                "created by a Big Endian version of Blender, support for these files has been removed in Blender 5.0"
            ),
            # io_tests/blend_big_endian/1.69/s-mesh.blend
            # File generated from a big endian build of Blender.
            "s-mesh.blend": (
                (OSError, RuntimeError),
                "created by a Big Endian version of Blender, support for these files has been removed in Blender 5.0"
            ),
            # io_tests/blend_big_endian/1.70/escher.blend
            # File generated from a big endian build of Blender.
            "escher.blend": (
                (OSError, RuntimeError),
                "created by a Big Endian version of Blender, support for these files has been removed in Blender 5.0"
            ),
            # io_tests/blend_big_endian/1.98/egypt.blend
            # File generated from a big endian build of Blender.
            "egypt.blend": (
                (OSError, RuntimeError),
                "created by a Big Endian version of Blender, support for these files has been removed in Blender 5.0"
            ),
            # io_tests/blend_big_endian/2.25/FroggyPacked.blend
            # File generated from a big endian build of Blender.
            "FroggyPacked.blend": (
                (OSError, RuntimeError),
                "created by a Big Endian version of Blender, support for these files has been removed in Blender 5.0"
            ),
            # io_tests/blend_big_endian/2.30/CtrlObject.blend
            # File generated from a big endian build of Blender.
            "CtrlObject.blend": (
                (OSError, RuntimeError),
                "created by a Big Endian version of Blender, support for these files has been removed in Blender 5.0"
            ),
            # io_tests/blend_big_endian/2.30/demofile.blend
            # File generated from a big endian build of Blender.
            "demofile.blend": (
                (OSError, RuntimeError),
                "created by a Big Endian version of Blender, support for these files has been removed in Blender 5.0"
            ),
            # io_tests/blend_big_endian/2.30/dolphin.blend
            # File generated from a big endian build of Blender.
            "dolphin.blend": (
                (OSError, RuntimeError),
                "created by a Big Endian version of Blender, support for these files has been removed in Blender 5.0"
            ),
            # io_tests/blend_big_endian/2.30/mball.blend
            # File generated from a big endian build of Blender.
            "mball.blend": (
                (OSError, RuntimeError),
                "created by a Big Endian version of Blender, support for these files has been removed in Blender 5.0"
            ),
            # io_tests/blend_big_endian/2.30/motor9.blend
            # File generated from a big endian build of Blender.
            "motor9.blend": (
                (OSError, RuntimeError),
                "created by a Big Endian version of Blender, support for these files has been removed in Blender 5.0"
            ),
            # io_tests/blend_big_endian/2.30/relative.blend
            # File generated from a big endian build of Blender.
            "relative.blend": (
                (OSError, RuntimeError),
                "created by a Big Endian version of Blender, support for these files has been removed in Blender 5.0"
            ),
            # io_tests/blend_big_endian/2.31/Raptor.blend
            # File generated from a big endian build of Blender.
            "Raptor.blend": (
                (OSError, RuntimeError),
                "created by a Big Endian version of Blender, support for these files has been removed in Blender 5.0"
            ),
            # io_tests/blend_big_endian/2.31/allselect.blend
            # File generated from a big endian build of Blender.
            "allselect.blend": (
                (OSError, RuntimeError),
                "created by a Big Endian version of Blender, support for these files has been removed in Blender 5.0"
            ),
            # io_tests/blend_big_endian/2.31/arealight.blend
            # File generated from a big endian build of Blender.
            "arealight.blend": (
                (OSError, RuntimeError),
                "created by a Big Endian version of Blender, support for these files has been removed in Blender 5.0"
            ),
            # io_tests/blend_big_endian/2.31/hairball.blend
            # File generated from a big endian build of Blender.
            "hairball.blend": (
                (OSError, RuntimeError),
                "created by a Big Endian version of Blender, support for these files has been removed in Blender 5.0"
            ),
            # io_tests/blend_big_endian/2.31/luxo.blend
            # File generated from a big endian build of Blender.
            "luxo.blend": (
                (OSError, RuntimeError),
                "created by a Big Endian version of Blender, support for these files has been removed in Blender 5.0"
            ),
            # io_tests/blend_big_endian/2.31/monkey_cornelius.blend
            # File generated from a big endian build of Blender.
            "monkey_cornelius.blend": (
                (OSError, RuntimeError),
                "created by a Big Endian version of Blender, support for these files has been removed in Blender 5.0"
            ),
            # io_tests/blend_big_endian/2.31/refract_monkey.blend
            # File generated from a big endian build of Blender.
            "refract_monkey.blend": (
                (OSError, RuntimeError),
                "created by a Big Endian version of Blender, support for these files has been removed in Blender 5.0"
            ),
            # io_tests/blend_big_endian/2.31/robo.blend
            # File generated from a big endian build of Blender.
            "robo.blend": (
                (OSError, RuntimeError),
                "created by a Big Endian version of Blender, support for these files has been removed in Blender 5.0"
            ),
            # io_tests/blend_big_endian/2.34/flippedmatrixes.blend
            # File generated from a big endian build of Blender.
            "flippedmatrixes.blend": (
                (OSError, RuntimeError),
                "created by a Big Endian version of Blender, support for these files has been removed in Blender 5.0"
            ),
            # io_tests/blend_big_endian/2.34/lostride.blend
            # File generated from a big endian build of Blender.
            "lostride.blend": (
                (OSError, RuntimeError),
                "created by a Big Endian version of Blender, support for these files has been removed in Blender 5.0"
            ),
            # io_tests/blend_big_endian/2.34/tapercurve.blend
            # File generated from a big endian build of Blender.
            "tapercurve.blend": (
                (OSError, RuntimeError),
                "created by a Big Endian version of Blender, support for these files has been removed in Blender 5.0"
            ),
            # io_tests/blend_big_endian/2.36/pathdist.blend
            # File generated from a big endian build of Blender.
            "pathdist.blend": (
                (OSError, RuntimeError),
                "created by a Big Endian version of Blender, support for these files has been removed in Blender 5.0"
            ),
            # io_tests/blend_big_endian/2.76/bird_sintel.blend
            # File generated from a big endian build of Blender.
            "bird_sintel.blend": (
                (OSError, RuntimeError),
                "created by a Big Endian version of Blender, support for these files has been removed in Blender 5.0"
            ),
        }

        assert all(p.endswith("/") for p in self.excluded_open_link_dirs)
        self.excluded_open_link_dirs = tuple(p.replace("/", os.sep) for p in self.excluded_open_link_dirs)
        assert all(p.endswith("/") for p in self.excluded_save_reload_dirs)
        self.excluded_save_reload_dirs = tuple(p.replace("/", os.sep) for p in self.excluded_save_reload_dirs)

        # Generate the slice of blendfile paths that this instance of the test should process.
        blendfile_paths = [p for p in self.iter_blendfiles_from_directory(self.args.src_test_dir)]
        # `os.scandir()` used by `iter_blendfiles_from_directory` does not
        # guarantee any form of order.
        blendfile_paths.sort()
        slice_indices = self.generate_slice_indices(len(blendfile_paths), self.args.slice_range, self.args.slice_index)
        self.blendfile_paths = blendfile_paths[slice_indices[0]:slice_indices[1]]

    @classmethod
    def iter_blendfiles_from_directory(cls, root_path):
        for dir_entry in os.scandir(root_path):
            if dir_entry.is_dir(follow_symlinks=False):
                yield from cls.iter_blendfiles_from_directory(dir_entry.path)
            elif dir_entry.is_file(follow_symlinks=False):
                if os.path.splitext(dir_entry.path)[1] == ".blend":
                    yield dir_entry.path

    @staticmethod
    def generate_slice_indices(total_len, slice_range, slice_index):
        slice_stride_base = total_len // slice_range
        slice_stride_remain = total_len % slice_range

        def gen_indices(i):
            return (
                (i * (slice_stride_base + 1))
                if i < slice_stride_remain else
                (slice_stride_remain * (slice_stride_base + 1)) + ((i - slice_stride_remain) * slice_stride_base)
            )
        slice_indices = [(gen_indices(i), gen_indices(i + 1)) for i in range(slice_range)]
        return slice_indices[slice_index]

    def skip_path_check(self, bfp, excluded_paths, excluded_dirs):
        if os.path.basename(bfp) in excluded_paths:
            return True
        if excluded_dirs:
            assert bfp.startswith(self.args.src_test_dir)
            bfp_relative = bfp[len(self.args.src_test_dir):].rstrip(os.sep)
            if bfp_relative.startswith(*excluded_dirs):
                return True
        return False

    def skip_open_link_path_check(self, bfp):
        return self.skip_path_check(bfp, self.excluded_open_link_paths, self.excluded_open_link_dirs)

    def skip_save_reload_path_check(self, bfp):
        return self.skip_path_check(bfp, self.excluded_save_reload_paths, self.excluded_save_reload_dirs)

    def invalid_path_exception_process(self, bfp, exception):
        expected_failure = self.invalid_paths.get(os.path.basename(bfp), None)
        if not expected_failure:
            raise exception
        # Check expected exception type(s).
        if not isinstance(exception, expected_failure[0]):
            raise exception
        # Check expected exception (partial) message.
        if expected_failure[1] not in str(exception):
            raise exception
        print(f"\tExpected failure: '{exception}'", flush=True)

    def save_reload(self, bfp, prefix):
        if self.skip_save_reload_path_check(bfp):
            return
        tmp_save_path = os.path.join(self.args.output_dir, prefix + os.path.basename(bfp))
        if not self.args.is_quiet:
            print(f"Trying to save to {tmp_save_path}", flush=True)
        bpy.ops.wm.save_as_mainfile(filepath=tmp_save_path, compress=True)
        if not self.args.is_quiet:
            print(f"Trying to reload from {tmp_save_path}", flush=True)
        bpy.ops.wm.revert_mainfile()
        if not self.args.is_quiet:
            print(f"Removing {tmp_save_path}", flush=True)
        bpy.ops.wm.read_homefile(use_empty=True, use_factory_startup=True)
        os.remove(tmp_save_path)

    def test_open(self):
        for bfp in self.blendfile_paths:
            if self.skip_open_link_path_check(bfp):
                continue
            if not self.args.is_quiet:
                print(f"Trying to open {bfp}", flush=True)
            bpy.ops.wm.read_homefile(use_empty=True, use_factory_startup=True)
            try:
                bpy.ops.wm.open_mainfile(filepath=bfp, load_ui=False)
                self.save_reload(bfp, "OPENED_")
            except BaseException as e:
                self.invalid_path_exception_process(bfp, e)

    def link_append(self, do_link):
        operation_name = "link" if do_link else "append"
        for bfp in self.blendfile_paths:
            if self.skip_open_link_path_check(bfp):
                continue
            bpy.ops.wm.read_homefile(use_empty=True, use_factory_startup=True)
            try:
                with bpy.data.libraries.load(bfp, link=do_link) as (lib_in, lib_out):
                    if len(lib_in.collections):
                        if not self.args.is_quiet:
                            print(f"Trying to {operation_name} {bfp}/Collection/{lib_in.collections[0]}", flush=True)
                        lib_out.collections.append(lib_in.collections[0])
                    elif len(lib_in.objects):
                        if not self.args.is_quiet:
                            print(f"Trying to {operation_name} {bfp}/Object/{lib_in.objects[0]}", flush=True)
                        lib_out.objects.append(lib_in.objects[0])
                self.save_reload(bfp, f"{operation_name.upper()}_")
            except BaseException as e:
                self.invalid_path_exception_process(bfp, e)

    def test_link(self):
        self.link_append(do_link=True)

    def test_append(self):
        self.link_append(do_link=False)


TESTS = (
    TestBlendFileOpenLinkSaveAllTestFiles,
)


def argparse_create():
    import argparse

    # When --help or no args are given, print this help
    description = ("Test basic versioning and writing code by opening, linking from, saving and reloading"
                   "all blend files in `--src-test-dir` directory (typically the `tests/files` one).")
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument(
        "--src-test-dir",
        dest="src_test_dir",
        default="..",
        help="Root tests directory to search for blendfiles",
        required=False,
    )
    parser.add_argument(
        "--output-dir",
        dest="output_dir",
        default=".",
        help="Where to output temp saved blendfiles",
        required=False,
    )

    parser.add_argument(
        "--quiet",
        dest="is_quiet",
        type=bool,
        default=False,
        help="Whether to quiet prints of all blendfile read/link attempts",
        required=False,
    )

    parser.add_argument(
        "--slice-range",
        dest="slice_range",
        type=int,
        default=1,
        help="How many instances of this test are launched in parallel, the list of available blendfiles is then sliced "
             "and each instance only processes the part matching its given `--slice-index`.",
        required=False,
    )
    parser.add_argument(
        "--slice-index",
        dest="slice_index",
        type=int,
        default=0,
        help="The index of the slice in blendfiles that this instance should process."
             "Should always be specified when `--slice-range` > 1",
        required=False,
    )

    return parser


def main():
    args = argparse_create().parse_args()

    assert args.slice_range > 0
    assert 0 <= args.slice_index < args.slice_range

    for Test in TESTS:
        Test(args).run_all_tests()


if __name__ == '__main__':
    import sys
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    main()
