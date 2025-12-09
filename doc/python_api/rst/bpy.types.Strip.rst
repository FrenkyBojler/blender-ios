bpy.types.Strip
===============

Sequence strip in the sequence editor.

Properties
----------

blend_alpha
    Percentage of how much the strip’s colors affect other strips.
    Type: float in [0, 1], default 1.0

blend_type
    Method for controlling how the strip combines with other strips.
    Type: enum, default 'ALPHA_OVER'

channel
    Y position of the sequence strip.
    Type: int in [1, 128], default 0

frame_offset_start
    Number of frames to offset the start of the strip. Can be used to trim or shift the visible start.
    Type: float in [-inf, inf], default 0.0

frame_offset_end
    Number of frames to offset the end of the strip. Can be used to trim or shift the visible end.
    Type: float in [-inf, inf], default 0.0

# (Add other properties similarly if you like)

Methods
-------

split(frame, split_method, *, ignore_connections=False)
    Split the strip at the specified frame.

    Parameters:
        frame (int): Frame where to split the strip.
        ignore_connections (bool, optional): If True, don't propagate split to connected strips.

    Returns:
        Strip: The right-side strip resulting from the split.

swap(other)
    Swap this strip with another strip in the sequence editor.

    Parameters:
        other (Strip): The strip to swap with.

move_to_meta(meta_sequence)
    Move this strip into the given meta strip.

    Parameters:
        meta_sequence (Strip): Destination Meta Strip.

parent_meta()
    Returns the parent meta strip of this strip.

    Returns:
        Strip: The parent meta strip, or None if there is no parent.
