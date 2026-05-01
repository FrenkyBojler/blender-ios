# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import unittest
import bpy
import pathlib
import sys

args = None
testfile = "main.blend"


def open_test_file():
    bpy.ops.wm.open_mainfile(filepath=str(args.testdir / testfile))


# Provide a valid context override to run text editor operators
def text_editor_context_override(context, text):
    window = context.window if context.window else next(
        window for window in context.window_manager.windows if window.screen is not None)
    screen = context.screen if context.screen else window.screen
    area = next(area for area in screen.areas if area.type == 'TEXT_EDITOR')
    region = next(region for region in area.regions if region.type == 'WINDOW')
    space = area.spaces[0]

    context_override = context.copy()
    context_override["window"] = window
    context_override["screen"] = screen
    context_override["area"] = area
    context_override["region"] = region
    context_override["space_data"] = space
    context_override["edit_text"] = text

    return context.temp_override(**context_override)

class AbstractNodeCopyOperatorTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        pass

    def setUp(self):
        self.assertTrue(args.testdir.exists(),
                        'Test dir {0} should exist'.format(args.testdir))
        open_test_file()

    def tearDown(self):
        pass

    def run_bracket_move_test(self, text_source, start_line, start_character, end_line, end_character):
        text = bpy.data.texts['Text']
        text.clear()
        text.write(text_source)
        text.cursor_set(start_line, character=start_character)

        # Move forward.
        with text_editor_context_override(bpy.context, text):
            bpy.ops.text.ensure_format()
            bpy.ops.text.move(type='BRACKET')

        self.assertEqual(text.current_line_index, end_line)
        self.assertEqual(text.current_character, end_character)

        # Move backward.
        with text_editor_context_override(bpy.context, text):
            bpy.ops.text.ensure_format()
            bpy.ops.text.move(type='BRACKET')

        self.assertEqual(text.current_line_index, start_line)
        self.assertEqual(text.current_character, start_character)

    def test_bracket_match_simple(self):
        self.run_bracket_move_test('def function():\n    pass', 0, 12, 0, 13)

    def test_bracket_match_newline(self):
        self.run_bracket_move_test('def function(\n):\n    pass', 0, 12, 1, 0)

    def test_bracket_match_tabs(self):
        # From #140973.
        self.run_bracket_move_test('\t\tvector VAO = N + nb + ( noise("perlin", P*(10000.0)) );', 0, 24, 0, 55)
        self.run_bracket_move_test('\t\tvector VAO = N + nb + ( noise("perlin", P*(10000.0)) );', 0, 31, 0, 53)
        self.run_bracket_move_test('\t\tvector VAO = N + nb + ( noise("perlin", P*(10000.0)) );', 0, 44, 0, 52)


def main():
    global args
    import argparse

    if '--' in sys.argv:
        argv = [sys.argv[0]] + sys.argv[sys.argv.index('--') + 1:]
    else:
        argv = sys.argv

    parser = argparse.ArgumentParser()
    parser.add_argument('--testdir', required=True, type=pathlib.Path)
    args, remaining = parser.parse_known_args(argv)

    unittest.main(argv=remaining)


if __name__ == "__main__":
    main()
