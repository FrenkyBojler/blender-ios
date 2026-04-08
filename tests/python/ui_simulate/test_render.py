# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
This file does not run anything, its methods are accessed for tests by ``run_blender_setup.py``.
"""
import modules.ui_test_utils as ui

# -----------------------------------------------------------------------------
# Viewport Rendering


def _interactive_rendering(engine):
    import bpy
    e, _, window = ui.test_window()

    rd = window.scene.render
    rd.engine = engine

    # Set up shading workspace with material editor and rendered viewport
    window.workspace = bpy.data.workspaces.get("Shading")
    yield
    properties_area = ui.get_window_area_by_type(window, 'PROPERTIES')
    properties_area.spaces.active.context = 'MATERIAL'
    view3d_area = ui.get_window_area_by_type(window, 'VIEW_3D')
    view3d_area.spaces.active.shading.type = 'RENDERED'
    yield

    # A few editing actions.
    def action_move():
        e.cursor_position_set(*ui.get_area_center(view3d_area), move=True)
        yield e.g().x().text("0.5").ret()

    def action_add_cube():
        e.cursor_position_set(*ui.get_area_center(view3d_area), move=True)
        bpy.ops.mesh.primitive_cube_add()
        yield

    def action_delete():
        e.cursor_position_set(*ui.get_area_center(view3d_area), move=True)
        yield e.delete().ret()

    def action_undo():
        yield e.ctrl.z()

    def action_redo():
        yield e.ctrl.shift.z()

    def action_add_node():
        ob = bpy.context.active_object
        tree = ob.active_material.node_tree
        tree.nodes.new(type="ShaderNodeMix")
        yield

    def action_unassign_material():
        ob = bpy.context.active_object
        ob.data.materials.pop()
        yield

    def action_assign_material():
        mat = bpy.data.materials.new(name="Test Material")
        ob = bpy.context.active_object
        ob.data.materials.append(mat)
        yield

    action_sequence = [
        action_move,
        action_add_cube,
        action_undo,
        action_redo,
        action_assign_material,
        action_add_node,
        action_unassign_material,
        action_delete,
    ]

    # Run actions one by one, for each giving a bit of time for
    # the interactive render to do some work.
    for action in action_sequence:
        yield from action()
        yield from ui.idle_until(lambda: False, timeout=0.2)


def interactive_rendering_cycles():
    yield from _interactive_rendering('CYCLES')


def interactive_rendering_eevee():
    yield from _interactive_rendering('BLENDER_EEVEE')

# -----------------------------------------------------------------------------
# Animation Rendering and Player


def _test_animation_player(t, window):
    # Launch animation player and verify it starts without crashing. It
    # doesn't support event simulation so not much we can test beyond that.
    import bpy
    import gpu
    import subprocess

    scene = window.scene
    rd = scene.render
    frame_path = rd.frame_path(frame=scene.frame_start)
    frame_path = bpy.path.abspath(frame_path)

    cmd = [
        bpy.app.binary_path,
        "--gpu-backend", gpu.platform.backend_type_get().lower(),
        "-a",
        "-f", str(rd.fps), str(rd.fps_base),
        "-s", str(scene.frame_start),
        "-e", str(scene.frame_end),
        "-j", str(scene.frame_step),
        frame_path,
    ]

    player_process = subprocess.Popen(cmd)

    # Wait a moment to verify it starts without immediately crashing.
    yield from ui.idle_until(lambda: False, timeout=2.0)
    t.assertIsNone(player_process.poll(), "Animation player process exited unexpectedly")

    # Terminate the player.
    player_process.terminate()
    try:
        player_process.wait(timeout=5.0)
    except subprocess.TimeoutExpired:
        player_process.kill()


def _animation_rendering_and_player(temp_dir):
    import bpy
    from pathlib import Path
    e, t, window = ui.test_window()

    # Change engine to Cycles and make it render quick.
    scene = window.scene
    rd = scene.render
    rd.engine = 'CYCLES'
    scene.cycles.samples = 1
    scene.cycles.use_denoising = False
    rd.resolution_x = 128
    rd.resolution_y = 128
    scene.frame_start = 1
    scene.frame_end = 20

    # Keyframe cube in two different locations.
    cube = bpy.data.objects.get("Cube")
    scene.frame_set(1)
    cube.location = (0, 0, 0)
    cube.keyframe_insert(data_path="location", frame=1)
    scene.frame_set(20)
    cube.location = (5, 5, 5)
    cube.keyframe_insert(data_path="location", frame=20)
    yield

    # Render animation and cancel after a few frames.
    rd.filepath = str(Path(temp_dir) / "final_")
    start_path = Path(bpy.path.abspath(rd.frame_path(frame=scene.frame_start)))

    bpy.ops.render.render('INVOKE_DEFAULT', animation=True)
    yield
    yield from ui.idle_until(
        lambda: start_path.exists() and scene.frame_current > 2,
        timeout=20.0)
    yield e.esc()
    yield from ui.idle_until(
        lambda: not bpy.app.is_job_running('RENDER'),
        timeout=20.0)

    t.assertTrue(start_path.exists(), "Start frame was not rendered")

    # In 3D viewport, render complete playblast.
    rd.filepath = str(Path(temp_dir) / "playblast_")
    start_path = Path(bpy.path.abspath(rd.frame_path(frame=scene.frame_start)))
    end_path = Path(bpy.path.abspath(rd.frame_path(frame=scene.frame_end)))

    view3d_area = ui.get_window_area_by_type(window, 'VIEW_3D')
    e.cursor_position_set(*ui.get_area_center(view3d_area), move=True)
    yield from ui.call_menu(e, "Render Playblast")
    yield from ui.idle_until(
        lambda: end_path.exists() and not bpy.app.is_job_running('RENDER'),
        timeout=20.0)

    t.assertTrue(start_path.exists(), "Start frame was not rendered")
    t.assertTrue(end_path.exists(), "End frame was not rendered")

    # Test animation player.
    yield from _test_animation_player(t, window)
    yield


def animation_rendering_and_player():
    import tempfile
    with tempfile.TemporaryDirectory(prefix="blender_test_render_") as temp_dir:
        yield from _animation_rendering_and_player(temp_dir)


# -----------------------------------------------------------------------------
# Render Data Override
#
# Ensure operators correctly use render settings which are set when the operator is invoked.

def _render_override_renderdata(kind, write_still, temp_dir):
    """
    Verify that overriding render output settings, starting a render operator (non-blocking)
    then restoring - properly overrides.

    :arg kind: 'GPU' for ``bpy.ops.render.opengl``, 'ENGINE' for ``bpy.ops.render.render``.
       Note that internally these use different code paths - why we test both.
    :arg write_still: When True, render a single still (``write_still=True``).
        When False, render an animation.
    """
    import bpy
    import contextlib
    import imbuf
    import os

    @contextlib.contextmanager
    def backup_attrs_multi(*obj_attrs):
        """
        Save the named attributes on each object and restore them on exit.
        Each argument is a ``(obj, (attr, ...))`` pair.
        """
        saved = []
        for obj, attrs in obj_attrs:
            saved.append((obj, {attr: getattr(obj, attr) for attr in attrs}))
        try:
            yield
        finally:
            for obj, d in saved:
                for attr, value in d.items():
                    setattr(obj, attr, value)

    e, t, window = ui.test_window()

    scene = window.scene
    rd = scene.render

    # Use a simple render engine (only image output is detected).
    rd.engine = 'BLENDER_WORKBENCH'
    # Restore values are deliberately distinct from the overrides below so
    # we can differentiate between them and ensure overrides work.
    rd.resolution_x = 32
    rd.resolution_y = 32
    rd.image_settings.file_format = 'BMP'
    rd.use_border = False
    rd.use_crop_to_border = False
    # Frame range is set once and not overridden. Overriding
    # `frame_start`/`frame_end`/`frame_step`/`frame_current` is intentionally
    # not supported - it would require intrusive changes to the per-frame
    # iteration & depsgraph advance logic.
    scene.frame_start = 1
    scene.frame_end = 2

    output_dir_orig = os.path.join(temp_dir, "output_orig")
    output_dir_override = os.path.join(temp_dir, "output_override")
    os.mkdir(output_dir_orig)
    os.mkdir(output_dir_override)

    # Output path the script will "restore" to. Tests assert nothing lands here.
    rd.filepath = os.path.join(output_dir_orig, "orig_")

    # - Override values used during rendering.
    # - On exit from the context manager they are restored.
    # - This happens *while* the render is still in progress.
    # - Failure to use the settings at the time the operator is invoked causes the tests to fail.
    override_resolution = (80, 48)
    # The engine path renders the bottom-left quarter with crop, so the output
    # image is half the resolution on each axis. The OpenGL/`render.opengl`
    # path doesn't support border-with-crop, so it produces the full resolution.
    image_size_expected = (40, 24) if kind == 'ENGINE' else override_resolution
    with backup_attrs_multi(
            (rd, (
                "filepath",
                "resolution_x",
                "resolution_y",
                "use_border",
                "use_crop_to_border",
                "border_min_x",
                "border_max_x",
                "border_min_y",
                "border_max_y",
            )),
            (rd.image_settings, (
                "file_format",
            )),
    ):
        rd.image_settings.file_format = 'PNG'
        rd.resolution_x = override_resolution[0]
        rd.resolution_y = override_resolution[1]
        rd.use_border = True
        rd.use_crop_to_border = True
        rd.border_min_x = 0.0
        rd.border_max_x = 0.5
        rd.border_min_y = 0.0
        rd.border_max_y = 0.5
        if write_still:
            rd.filepath = os.path.join(output_dir_override, "still")
            filenames_expected = (
                "still.png",
            )
        else:
            rd.filepath = os.path.join(output_dir_override, "anim_")
            filenames_expected = (
                "anim_0001.png",
                "anim_0002.png",
            )

        if kind == 'GPU':
            bpy.ops.render.opengl(
                'INVOKE_DEFAULT', animation=not write_still, write_still=write_still)
        elif kind == 'ENGINE':
            bpy.ops.render.render(
                'INVOKE_DEFAULT', animation=not write_still, write_still=write_still)
        else:
            assert False, "Unreachable!"

    # GPU still rendering happens in the modal handler which only fires on
    # real window events. Inject a cursor motion to drive it. (Harmless for
    # the other paths.)
    view3d_area = ui.get_window_area_by_type(window, 'VIEW_3D')
    e.cursor_position_set(*ui.get_area_center(view3d_area), move=True)
    yield

    yield from ui.idle_until(
        lambda: not bpy.app.is_job_running('RENDER'),
        timeout=30.0,
    )

    filenames_actual = tuple(sorted(os.listdir(output_dir_override)))
    t.assertEqual(filenames_actual, filenames_expected)

    for filename in filenames_expected:
        path = os.path.join(output_dir_override, filename)
        ibuf = imbuf.load(path)
        try:
            t.assertEqual(ibuf.size, image_size_expected)
            t.assertEqual(ibuf.file_type, 'PNG')
        finally:
            ibuf.free()

    # Hygiene: nothing should have leaked into the original output dir.
    leaked = tuple(sorted(os.listdir(output_dir_orig)))
    t.assertEqual(leaked, ())


def render_override_renderdata_gpu_animation():
    import tempfile
    with tempfile.TemporaryDirectory(prefix="blender_test_render_override_") as temp_dir:
        yield from _render_override_renderdata('GPU', False, temp_dir)


def render_override_renderdata_gpu_still():
    import tempfile
    with tempfile.TemporaryDirectory(prefix="blender_test_render_override_") as temp_dir:
        yield from _render_override_renderdata('GPU', True, temp_dir)


def render_override_renderdata_engine_animation():
    import tempfile
    with tempfile.TemporaryDirectory(prefix="blender_test_render_override_") as temp_dir:
        yield from _render_override_renderdata('ENGINE', False, temp_dir)


def render_override_renderdata_engine_still():
    import tempfile
    with tempfile.TemporaryDirectory(prefix="blender_test_render_override_") as temp_dir:
        yield from _render_override_renderdata('ENGINE', True, temp_dir)
