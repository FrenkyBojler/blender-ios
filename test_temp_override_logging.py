#!/usr/bin/env python3

import bpy


def test_temp_override_logging():
    """Test script to demonstrate temp_override logging of context member access."""

    print("=" * 50)
    print("Testing temp_override context member access logging")
    print("=" * 50)

    # First, access some context members normally (should not log)
    print("\n1. Normal context access (should not log):")
    print("   - Accessing context.scene...")
    scene = bpy.context.scene
    print("   - Accessing context.view_layer...")
    view_layer = bpy.context.view_layer
    print("   - Accessing context.object...")
    obj = bpy.context.object

    print("\n2. Context access within temp_override (should log):")
    print("   - Entering temp_override...")

    # Now test with temp_override - this should log member accesses
    with bpy.context.temp_override():
        print("   - Accessing context.scene inside temp_override...")
        scene = bpy.context.scene
        print("   - Accessing context.view_layer inside temp_override...")
        view_layer = bpy.context.view_layer
        print("   - Accessing context.object inside temp_override...")
        obj = bpy.context.object
        print("   - Accessing context.active_object inside temp_override...")
        active_obj = bpy.context.active_object

    print("   - Exited temp_override")

    print("\n3. Normal context access again (should not log):")
    print("   - Accessing context.scene...")
    scene = bpy.context.scene
    print("   - Accessing context.view_layer...")
    view_layer = bpy.context.view_layer

    print("\n" + "=" * 50)
    print("Test complete! Check console output for logged context member accesses.")
    print("=" * 50)


if __name__ == "__main__":
    test_temp_override_logging()
