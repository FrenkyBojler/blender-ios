"""
**Logging Context Member Access**

Context members can be logged by calling ``logging_set(True)`` on the with-target of a temporary
override. This will create a log of the members that are being accessed during the operation and may
assist in debugging when it is unclear which members need to be overridden.

In the event an operator fails to execute because of a missing context member, logging may help
identify which member is required.

This example shows how to log which context members are being accessed. Log statements are
printed to your system's console.
"""

import bpy
from bpy import context

my_objs = [bpy.data.objects['Cube']]

with context.temp_override(selected_objects=my_objs) as override_target:
    override_target.logging_set(True)  # Enable logging
    bpy.ops.object.delete()
