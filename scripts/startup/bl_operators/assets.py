# SPDX-FileCopyrightText: 2020-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Asset Browser and Library operators.

This module contains operators for:
- Asset Browser (ASSET_OT_*): Tag management and asset file operations
- Outliner Libraries (OUTLINER_OT_*): Library blend file operations

Both ASSET_OT and OUTLINER_OT operators are kept together as they share
common functionality for opening blend files in new Blender instances.
"""

from __future__ import annotations

import bpy
from bpy.types import Operator
from bpy.props import StringProperty
from bpy.app.translations import (
    pgettext_data as data_,
    pgettext_rpt as rpt_,
)


from bpy_extras.asset_utils import (
    SpaceAssetInfo,
)


class BlendFileOpener:
    """Base class for operators that open blend files in new Blender instances"""

    _process = None  # Optional[subprocess.Popen]

    def open_in_new_blender(self, filepath):
        """Open a blend file in a new Blender instance"""
        import subprocess
        cli_args = [bpy.app.binary_path, str(filepath)]
        self._process = subprocess.Popen(cli_args)

    def modal(self, context, event):
        """Monitor the subprocess until it completes"""
        if event.type != 'TIMER':
            return {'PASS_THROUGH'}

        if self._process is None:
            self.report({'ERROR'}, "Unable to find any running process")
            self.cancel(context)
            return {'CANCELLED'}

        returncode = self._process.poll()
        if returncode is None:
            # Process is still running
            return {'RUNNING_MODAL'}

        if returncode:
            self.report({'WARNING'},
                        rpt_("Blender sub-process exited with error code {:d}").format(returncode))

        # Allow subclasses to add cleanup logic
        self.on_process_finished(context)

        self.cancel(context)
        return {'FINISHED'}

    def cancel(self, context):
        """Clean up timer when operation is cancelled"""
        wm = context.window_manager
        wm.event_timer_remove(self._timer)

    def on_process_finished(self, context):
        """Override in subclass to add cleanup logic after process finishes"""
        pass


class AssetBrowserMetadataOperator:
    @classmethod
    def poll(cls, context):
        if not SpaceAssetInfo.is_asset_browser_poll(context) or not context.asset:
            return False

        if not context.asset.local_id:
            Operator.poll_message_set(
                "Asset metadata from external asset libraries cannot be "
                "edited, only assets stored in the current file can"
            )
            return False
        return True


class ASSET_OT_tag_add(AssetBrowserMetadataOperator, Operator):
    """Add a new keyword tag to the active asset"""

    bl_idname = "asset.tag_add"
    bl_label = "Add Asset Tag"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        active_asset = context.asset
        active_asset.metadata.tags.new(data_("Tag"))

        return {'FINISHED'}


class ASSET_OT_tag_remove(AssetBrowserMetadataOperator, Operator):
    """Remove an existing keyword tag from the active asset"""

    bl_idname = "asset.tag_remove"
    bl_label = "Remove Asset Tag"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        if not super().poll(context):
            return False

        active_asset = context.asset
        asset_metadata = active_asset.metadata
        return asset_metadata.active_tag in range(len(asset_metadata.tags))

    def execute(self, context):
        active_asset = context.asset
        asset_metadata = active_asset.metadata
        tag = asset_metadata.tags[asset_metadata.active_tag]

        asset_metadata.tags.remove(tag)
        asset_metadata.active_tag -= 1

        return {'FINISHED'}


class ASSET_OT_open_containing_blend_file(BlendFileOpener, Operator):
    """Open the blend file that contains the active asset"""

    bl_idname = "asset.open_containing_blend_file"
    bl_label = "Open Blend File"
    bl_options = {'REGISTER'}

    @classmethod
    def poll(cls, context):
        asset = getattr(context, "asset", None)

        if not asset:
            cls.poll_message_set("No asset selected")
            return False
        if asset.local_id:
            cls.poll_message_set("Selected asset is contained in the current file")
            return False
        if asset.is_online:
            cls.poll_message_set("Selected asset is stored online")
            return False
        # This could become a built-in query, for now this is good enough.
        if asset.full_library_path.endswith(".asset.blend"):
            cls.poll_message_set(
                "Selected asset is contained in a file managed by the asset system, manual edits should be avoided",
            )
            return False
        return True

    def execute(self, context):
        asset = context.asset

        if asset.local_id:
            self.report({'WARNING'}, "This asset is stored in the current blend file")
            return {'CANCELLED'}

        asset_lib_path = asset.full_library_path
        self.open_in_new_blender(asset_lib_path)

        wm = context.window_manager
        self._timer = wm.event_timer_add(0.1, window=context.window)
        wm.modal_handler_add(self)

        return {'RUNNING_MODAL'}

    def on_process_finished(self, context):
        """Refresh asset library after opening the file"""
        if bpy.ops.asset.library_refresh.poll():
            bpy.ops.asset.library_refresh()


class OUTLINER_OT_library_open_blend_file(BlendFileOpener, Operator):
    """Open the blend file of the selected library in a new Blender instance

    This is a parametrized operator that accepts filepath via property.
    Unlike ASSET_OT_open_containing_blend_file which gets path from context.asset,
    this operator is designed to be called from multiple places (UI and C++)
    with filepath passed as a parameter.
    """

    bl_idname = "outliner.library_open_blend_file"
    bl_label = "Open Blend File"
    bl_options = {'REGISTER', 'INTERNAL'}

    filepath: StringProperty(
        name="Library Path",
        description="Path to the library blend file",
        subtype='FILE_PATH'
    )

    @classmethod
    def poll(cls, context):
        # Operator is available when called with filepath parameter
        return True

    def execute(self, context):
        if not self.filepath:
            self.report({'ERROR'}, "No library filepath provided")
            return {'CANCELLED'}

        # Check if file exists
        import os
        if not os.path.exists(self.filepath):
            self.report({'ERROR'}, f"Library file not found: {self.filepath}")
            return {'CANCELLED'}

        self.open_in_new_blender(self.filepath)

        wm = context.window_manager
        self._timer = wm.event_timer_add(0.1, window=context.window)
        wm.modal_handler_add(self)

        return {'RUNNING_MODAL'}


classes = (
    ASSET_OT_tag_add,
    ASSET_OT_tag_remove,
    ASSET_OT_open_containing_blend_file,
    OUTLINER_OT_library_open_blend_file,
)
