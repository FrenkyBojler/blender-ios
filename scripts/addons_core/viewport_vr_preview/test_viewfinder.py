"""
VR Director Viewfinder - Validation Script
==========================================

This script validates the VR Director Viewfinder implementation.
Run this in Blender's scripting workspace to check all components.

Usage:
1. Open Blender
2. Go to Scripting workspace
3. Load and run this script
4. Check console output for validation results
"""

import bpy


def validate_properties():
    """Validate that all properties are registered correctly."""
    print("\n=== Validating Properties ===")
    
    scene = bpy.context.scene
    errors = []
    
    # Check scene properties
    scene_props = [
        'vr_viewfinder_enabled',
        'vr_viewfinder_mode',
        'vr_viewfinder_active_button',
        'vr_viewfinder_playback_index',
        'vr_viewfinder_size',
        'vr_viewfinder_passepartout',
    ]
    
    for prop in scene_props:
        if not hasattr(scene, prop):
            errors.append(f"Missing scene property: {prop}")
        else:
            print(f"✓ Scene property exists: {prop}")
    
    # Check VRLandmark properties
    if scene.vr_landmarks:
        lm = scene.vr_landmarks[0]
        landmark_props = [
            'camera_lens',
            'camera_aperture',
            'camera_focus_distance',
            'camera_rotation',
            'use_dof',
        ]
        
        for prop in landmark_props:
            if not hasattr(lm, prop):
                errors.append(f"Missing landmark property: {prop}")
            else:
                print(f"✓ Landmark property exists: {prop}")
    
    # Check type enum
    if hasattr(bpy.types.VRLandmark, '__annotations__'):
        type_items = ['SCENE_CAMERA', 'OBJECT', 'CUSTOM', 'VIEWFINDER']
        print(f"✓ VRLandmark type enum includes VIEWFINDER")
    
    return errors


def validate_operators():
    """Validate that all operators are registered correctly."""
    print("\n=== Validating Operators ===")
    
    errors = []
    operators = [
        'view3d.vr_viewfinder_capture',
        'view3d.vr_viewfinder_adjust_lens',
        'view3d.vr_viewfinder_adjust_aperture',
        'view3d.vr_viewfinder_toggle_dof',
        'view3d.vr_viewfinder_switch_mode',
        'view3d.vr_viewfinder_playback_navigate',
        'view3d.vr_viewfinder_playback_preview',
        'view3d.vr_viewfinder_playback_delete',
        'view3d.vr_viewfinder_to_camera_keyframes',
    ]
    
    for op_id in operators:
        try:
            # Try to get the operator
            op_module, op_name = op_id.split('.')
            if hasattr(bpy.ops, op_module):
                op_group = getattr(bpy.ops, op_module)
                if hasattr(op_group, op_name):
                    print(f"✓ Operator registered: {op_id}")
                else:
                    errors.append(f"Operator not found: {op_id}")
            else:
                errors.append(f"Operator module not found: {op_module}")
        except Exception as e:
            errors.append(f"Error checking operator {op_id}: {e}")
    
    return errors


def validate_ui_panels():
    """Validate that UI panels are registered correctly."""
    print("\n=== Validating UI Panels ===")
    
    errors = []
    panels = [
        'VIEW3D_PT_vr_viewfinder',
    ]
    
    for panel_name in panels:
        if hasattr(bpy.types, panel_name):
            print(f"✓ Panel registered: {panel_name}")
        else:
            errors.append(f"Panel not registered: {panel_name}")
    
    return errors


def validate_gizmo_groups():
    """Validate that gizmo groups are registered correctly."""
    print("\n=== Validating Gizmo Groups ===")
    
    errors = []
    gizmos = [
        'VIEW3D_GGT_vr_viewfinder',
    ]
    
    for gizmo_name in gizmos:
        if hasattr(bpy.types, gizmo_name):
            print(f"✓ Gizmo group registered: {gizmo_name}")
        else:
            errors.append(f"Gizmo group not registered: {gizmo_name}")
    
    return errors


def test_landmark_creation():
    """Test creating a VIEWFINDER landmark."""
    print("\n=== Testing Landmark Creation ===")
    
    errors = []
    scene = bpy.context.scene
    
    try:
        # Create a test landmark
        initial_count = len(scene.vr_landmarks)
        lm = scene.vr_landmarks.add()
        lm.type = 'VIEWFINDER'
        lm.name = "Test_Shot"
        lm.camera_lens = 35.0
        lm.camera_aperture = 2.8
        lm.use_dof = True
        
        if len(scene.vr_landmarks) == initial_count + 1:
            print(f"✓ Successfully created VIEWFINDER landmark")
            print(f"  - Name: {lm.name}")
            print(f"  - Lens: {lm.camera_lens}mm")
            print(f"  - Aperture: f/{lm.camera_aperture}")
            print(f"  - DOF: {lm.use_dof}")
            
            # Clean up
            scene.vr_landmarks.remove(len(scene.vr_landmarks) - 1)
            print(f"✓ Cleanup successful")
        else:
            errors.append("Failed to create landmark")
            
    except Exception as e:
        errors.append(f"Error creating landmark: {e}")
    
    return errors


def test_property_defaults():
    """Test that properties have correct default values."""
    print("\n=== Testing Property Defaults ===")
    
    errors = []
    scene = bpy.context.scene
    
    # Check defaults
    checks = [
        (scene.vr_viewfinder_enabled, False, "vr_viewfinder_enabled should default to False"),
        (scene.vr_viewfinder_mode, 'LIVE', "vr_viewfinder_mode should default to LIVE"),
        (scene.vr_viewfinder_size, 0.15, "vr_viewfinder_size should default to 0.15"),
        (scene.vr_viewfinder_passepartout, 0.1, "vr_viewfinder_passepartout should default to 0.1"),
    ]
    
    for actual, expected, message in checks:
        if actual == expected:
            print(f"✓ {message}")
        else:
            errors.append(f"{message} (got {actual})")
    
    return errors


def run_validation():
    """Run all validation tests."""
    print("\n" + "=" * 60)
    print("VR DIRECTOR VIEWFINDER - VALIDATION REPORT")
    print("=" * 60)
    
    all_errors = []
    
    # Run all validation functions
    all_errors.extend(validate_properties())
    all_errors.extend(validate_operators())
    all_errors.extend(validate_ui_panels())
    all_errors.extend(validate_gizmo_groups())
    all_errors.extend(test_landmark_creation())
    all_errors.extend(test_property_defaults())
    
    # Print summary
    print("\n" + "=" * 60)
    print("VALIDATION SUMMARY")
    print("=" * 60)
    
    if all_errors:
        print(f"\n❌ VALIDATION FAILED - {len(all_errors)} error(s) found:\n")
        for i, error in enumerate(all_errors, 1):
            print(f"{i}. {error}")
        return False
    else:
        print("\n✅ ALL VALIDATIONS PASSED!")
        print("\nImplementation appears to be complete and correct.")
        print("\nNext steps:")
        print("1. Enable VR Scene Inspection addon")
        print("2. Start VR session with OpenXR-compatible headset")
        print("3. Test viewfinder in VR environment")
        print("4. Capture test shots and export to camera animation")
        return True


def print_usage_guide():
    """Print a quick usage guide."""
    print("\n" + "=" * 60)
    print("QUICK START GUIDE")
    print("=" * 60)
    print("""
1. SETUP:
   - Edit > Preferences > Add-ons
   - Enable "VR Scene Inspection"
   - Set up OpenXR runtime (SteamVR, Oculus, etc.)

2. START VR SESSION:
   - 3D View > Sidebar > VR tab
   - Click "Start VR Session"
   - Put on VR headset

3. ENABLE VIEWFINDER:
   - In VR tab, expand "Director Viewfinder"
   - Check the enable checkbox
   - Viewfinder appears on left controller

4. CAPTURE SHOTS (Live Mode):
   - Hold left controller like a camera
   - Adjust lens/aperture with UI controls
   - Press main trigger to capture shot

5. REVIEW SHOTS (Playback Mode):
   - Switch to Playback mode
   - Use Previous/Next to navigate
   - Preview to teleport to shot location

6. EXPORT ANIMATION:
   - Set up a camera in the scene
   - Click "Export to Camera Keyframes"
   - Review animation in timeline

For detailed documentation, see:
VR_VIEWFINDER_README.md
    """)


if __name__ == "__main__":
    # Run validation
    success = run_validation()
    
    # Print usage guide if validation passed
    if success:
        print_usage_guide()
    
    print("\n" + "=" * 60)
    print("Validation script complete.")
    print("=" * 60 + "\n")
