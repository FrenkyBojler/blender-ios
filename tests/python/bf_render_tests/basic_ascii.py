#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# ./blender.bin --background --factory-startup --python tests/python/bf_render_tests/basic_ascii.py -- /tmp/tmp_basic_ascii0001.png

import blf
import bpy
import imbuf
import sys


def render_font_test():
    """Render the font test and save to output_path."""
    
    output_path = sys.argv[sys.argv.index("--") + 1]

    # Simple hardcoded configuration
    image_size = (512, 128)
    font_size = 24
    text = "The quick brown fox jumps over the lazy dog."
    text_position = (10, 90)

    # Create image buffer
    ibuf = imbuf.new(image_size)

    # Use default built-in font
    font_id = 0

    # Configure font
    blf.color(font_id, 0.3, 0.3, 0.3, 1.0)  # Grey Text (readable on black and white background)
    blf.size(font_id, font_size)
    blf.position(font_id, text_position[0], text_position[1], 0)
    blf.disable(font_id, blf.WORD_WRAP)

    with blf.bind_imbuf(font_id, ibuf, display_name="sRGB"):
        blf.draw_buffer(font_id, text)

    print(f"Saving image to: {output_path}")
    imbuf.write(ibuf, filepath=output_path)


render_font_test()