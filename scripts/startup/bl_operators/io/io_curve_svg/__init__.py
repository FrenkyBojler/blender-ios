# SPDX-FileCopyrightText: 2011-2022 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later

import os
import bpy
from bpy.props import (
    StringProperty,
    CollectionProperty
)
from bpy_extras.io_utils import ImportHelper


class ImportSVG(bpy.types.Operator, ImportHelper):
    """Load a SVG file"""
    bl_idname = "import_curve.svg"
    bl_label = "Import SVG"
    bl_options = {'UNDO'}

    filename_ext = ".svg"
    filter_glob: StringProperty(default="*.svg", options={'HIDDEN'})

    files: CollectionProperty(
        name="File Path",
        type=bpy.types.OperatorFileListElement,
    )


    def execute(self, context):
        from . import import_svg

        if self.files:
            ret = {'CANCELLED'}
            dirname = os.path.dirname(self.filepath)
            for file in self.files:
                path = os.path.join(dirname, file.name)
                if import_svg.load(self, context, filepath=path) == {'FINISHED'}:
                    ret = {'FINISHED'}
            return ret
        else:
            return import_svg.load(self, context, filepath=self.filepath)


def register():
    bpy.utils.register_class(ImportSVG)

def unregister():
    bpy.utils.unregister_class(ImportSVG)

