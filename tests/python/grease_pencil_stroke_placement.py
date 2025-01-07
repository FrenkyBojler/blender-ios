import bpy
import unittest

import os, sys
directory = os.path.dirname(__file__)
sys.path.insert(0, directory)
from ui_simulate.modules import easy_keys

def _test_window(windows_exclude=None):
    wm = bpy.data.window_managers[0]
    # Use -1 so the last added window is always used.
    if windows_exclude is None:
        return wm.windows[0]
    for window in wm.windows:
        if window not in windows_exclude:
            return window

def _test_view3d(window):
    screen = window.screen
    for area in screen.areas:
        if area.type == 'VIEW_3D':
            return area

def _test_context(context, window, area):
    return context.temp_override(window=window, screen=window.screen, area=area, space_data=area.spaces.active)

def _call_menu(e, text: str):
    yield e.f3()
    yield e.text_unicode(text.replace(" -> ", " \u25b8 "))
    yield e.ret()


easy_keys.setup_default_preferences(bpy.context.preferences)

window = _test_window()
area = _test_view3d(window)

bpy.context.preferences.view.show_splash = False
# So we can get the operator ID's.
bpy.context.preferences.view.show_developer_ui = True

def test_events():
    # e, t = _test_vars(window)
    # yield from _view3d_startup_area_maximized(e)
    t = unittest.TestCase()
    e = easy_keys.EventGenerate(window)

    def coords(x, y):
        return (area.x + area.width // 2 + x, area.y + area.height // 2 + y)

    with _test_context(bpy.context, window, area):
        bpy.ops.object.select_all(action='SELECT')
        bpy.ops.object.delete(use_global=False)

        bpy.ops.object.grease_pencil_add(type='EMPTY', radius=1, align='WORLD', location=(0, 0, 0), scale=(1, 1, 1))
        bpy.ops.grease_pencil.paintmode_toggle()
        bpy.ops.wm.tool_set_by_id(name="builtin.box")
    yield

    # e.cursor_position_set(*coords(-100, -100))
    # yield
    # e.leftmouse.press()
    # yield
    # e.cursor_position_set(*coords(100, 100), move=True)
    # yield
    # e.leftmouse.release()
    # yield
    yield from e.leftmouse.cursor_motion([coords(-100, -100), coords(100, 100)])
    e.ret.tap()
    yield

easy_keys.run(
    test_events(),
    # on_error=on_error,
    # on_exit=on_exit,
    # Optional.
    # on_step_command_pre=args.step_command_pre,
    # on_step_command_post=args.step_command_post,
)
