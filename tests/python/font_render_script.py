  #!/usr/bin/env python3
# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

"""
Simple BLF font rendering script.

Renders basic ASCII text using BLF into an image buffer.
"""

import sys
import os
import bpy
import blf
import imbuf


def render_font_test(output_path):
    """Render the font test and save to output_path."""
    
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
    blf.color(font_id, 1.0, 1.0, 1.0, 1.0)  # White text
    blf.size(font_id, font_size)
    blf.position(font_id, text_position[0], text_position[1], 0)
    
    # Disable word wrapping for simple test
    blf.disable(font_id, blf.WORD_WRAP)
    
    # Render text into image buffer
    try:
        with blf.bind_imbuf(font_id, ibuf, display_name="sRGB"):
            blf.draw_buffer(font_id, text)
    except Exception as e:
        print(f"Error during text rendering: {e}")
        return False
    
    # Save image
    try:
        print(f"Saving image to: {output_path}")
        imbuf.write(ibuf, filepath=output_path)
        
        # Check if file was actually created (since imbuf.write may return False even on success)
        if os.path.exists(output_path):
            print(f"Successfully rendered font test to: {output_path}")
            return True
        else:
            print(f"Error: File was not created at {output_path}")
            return False
            
    except Exception as e:
        print(f"Error writing image: {e}")
        import traceback
        traceback.print_exc()
        return False


def main():
    """Main function for font rendering test script."""
    
    # Get output path from command line arguments
    if '--' in sys.argv:
        args = sys.argv[sys.argv.index('--') + 1:]
        if len(args) >= 1:
            output_path_base = args[0]
        else:
            print("Error: No output path provided")
            return 1
    else:
        print("Error: No output path provided")
        return 1
    
    # Add the frame number suffix (0001.png) as render_report expects
    output_path = output_path_base + '0001.png'
    
    # Ensure output directory exists
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    
    # Render the test
    success = render_font_test(output_path)
    
    if not success:
        return 1
    
    return 0


if __name__ == "__main__":
    sys.exit(main())