# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Screen-space mask gestures (P10 class-2: paint.mask_box_gesture /
paint.mask_lasso_gesture equivalents).

A gesture op runs a small modal machine: ARMED (invoked from a key, waits
for the first press) or straight to DRAG (invoked from a mouse-button
chord), draws a GPU rubber band, and on release projects every engine
vertex into region space and writes the mask for the verts inside the
region. The box op also applies from explicit ``xmin..ymax`` properties in
execute(), which keeps the projection/apply path testable without a modal
loop (same shape as vanilla's WM box gesture).

Differences from vanilla, recorded in the audit: no symmetry passes, no
front-faces-only option, and occluded (back-side) vertices are affected —
vanilla's default behaves the same way, but its option to limit is absent.
"""

import bpy

from . import convert, engine, undo
from .ops import _session

_MOUSE_BUTTONS = {'LEFTMOUSE', 'MIDDLEMOUSE', 'RIGHTMOUSE'}


def _project_verts(context, session):
    """Region-space (x, y) of every live engine vert + validity (w > 0)."""
    import numpy as np

    region = context.region
    rv3d = context.region_data
    ob = context.active_object
    positions = convert.mesh_positions(session.mesh_ptr).reshape(-1, 3)
    matrix = np.array(rv3d.perspective_matrix @ ob.matrix_world, dtype=np.float64)
    homo = np.empty((len(positions), 4), dtype=np.float64)
    homo[:, :3] = positions
    homo[:, 3] = 1.0
    clip = homo @ matrix.T
    w = clip[:, 3]
    valid = w > 1e-8
    xy = np.zeros((len(positions), 2), dtype=np.float64)
    xy[valid] = clip[valid, :2] / w[valid, None]
    xy[:, 0] = (xy[:, 0] * 0.5 + 0.5) * region.width
    xy[:, 1] = (xy[:, 1] * 0.5 + 0.5) * region.height
    return xy, valid


def _apply_mask(context, session, selected, mode, value, message):
    """Write ``value`` (or invert) into the mask for ``selected`` verts,
    with an attribute-snapshot undo step."""
    import numpy as np

    if not selected.any():
        return False
    lib = engine.capi().lib
    verts_num = convert.mesh_vert_num(session.mesh_ptr)
    before = np.zeros(verts_num, dtype=np.float32)
    lib.Mesh_readVertFloatAttr(session.mesh_ptr, convert._SC_MASK, before)
    after = before.copy()
    if mode == 'INVERT':
        after[selected] = 1.0 - after[selected]
    else:
        after[selected] = value
    lib.Mesh_writeVertFloatAttr(session.mesh_ptr, convert._SC_MASK,
                                np.ascontiguousarray(after))
    undo.push_attr(context, context.active_object, session, message,
                   'VERT_F32', convert._SC_MASK, before.tobytes(), after.tobytes())
    undo._tag_view3d_redraw(context)
    return True


def points_in_polygon(xy, valid, polygon):
    """Even-odd inside test for (N, 2) points against a polygon (point
    list), vectorized per polygon edge; a bbox prefilter keeps the edge
    loop cheap on large meshes."""
    import numpy as np

    poly = np.asarray(polygon, dtype=np.float64)
    x, y = xy[:, 0], xy[:, 1]
    candidate = (valid
                 & (x >= poly[:, 0].min()) & (x <= poly[:, 0].max())
                 & (y >= poly[:, 1].min()) & (y <= poly[:, 1].max()))
    cx, cy = x[candidate], y[candidate]
    inside_c = np.zeros(len(cx), dtype=bool)
    px, py = poly[:, 0], poly[:, 1]
    qx, qy = np.roll(px, -1), np.roll(py, -1)
    for i in range(len(poly)):
        if qy[i] == py[i]:
            continue  # horizontal edge: never crosses a scanline
        crosses = ((py[i] > cy) != (qy[i] > cy))
        if not crosses.any():
            continue
        t = (cy - py[i]) / (qy[i] - py[i])
        inside_c ^= crosses & (cx < px[i] + t * (qx[i] - px[i]))
    inside = np.zeros(len(xy), dtype=bool)
    inside[candidate] = inside_c
    return inside


class _MaskGesture:
    """Modal machinery shared by the box and lasso gestures."""

    mode: bpy.props.EnumProperty(
        name="Mode",
        items=(
            ('VALUE', "Value", "Set the mask to the given value"),
            ('INVERT', "Invert", "Invert the mask inside the gesture"),
        ),
        default='VALUE',
    )
    value: bpy.props.FloatProperty(name="Value", default=1.0, min=0.0, max=1.0)

    @classmethod
    def poll(cls, context):
        return _session(context) is not None

    def invoke(self, context, event):
        if context.region is None or context.region_data is None:
            return {'CANCELLED'}
        self._points = []
        if event.type in _MOUSE_BUTTONS and event.value == 'PRESS':
            # Chord-invoked (button already down): drag immediately.
            self._button = event.type
            self._points.append((event.mouse_region_x, event.mouse_region_y))
        else:
            # Key-invoked (e.g. B): arm and wait for the press.
            self._button = None
        context.window.cursor_modal_set('CROSSHAIR')
        self._handle = bpy.types.SpaceView3D.draw_handler_add(
            self._draw, (context,), 'WINDOW', 'POST_PIXEL')
        context.window_manager.modal_handler_add(self)
        return {'RUNNING_MODAL'}

    def modal(self, context, event):
        if event.type in {'ESC'} or (
                self._button is None and event.type == 'RIGHTMOUSE'):
            return self._finish(context, apply_gesture=False)
        if self._button is None:
            if event.type == 'LEFTMOUSE' and event.value == 'PRESS':
                self._button = 'LEFTMOUSE'
                self._points.append((event.mouse_region_x, event.mouse_region_y))
            return {'RUNNING_MODAL'}
        if event.type == 'MOUSEMOVE':
            self._track(event)
            context.area.tag_redraw()
            return {'RUNNING_MODAL'}
        if event.type == self._button and event.value == 'RELEASE':
            return self._finish(context, apply_gesture=True)
        return {'RUNNING_MODAL'}

    def _finish(self, context, apply_gesture):
        bpy.types.SpaceView3D.draw_handler_remove(self._handle, 'WINDOW')
        context.window.cursor_modal_restore()
        context.area.tag_redraw()
        if apply_gesture and len(self._points) >= 2:
            if self._apply(context):
                return {'FINISHED'}
        return {'CANCELLED'}

    def _draw(self, _context):
        import gpu
        from gpu_extras.batch import batch_for_shader

        outline = self._outline()
        if len(outline) < 2:
            return
        shader = gpu.shader.from_builtin('UNIFORM_COLOR')
        batch = batch_for_shader(shader, 'LINE_LOOP', {"pos": outline})
        shader.uniform_float("color", (1.0, 1.0, 1.0, 0.6))
        gpu.state.blend_set('ALPHA')
        batch.draw(shader)
        gpu.state.blend_set('NONE')


class SCULPTCORE_OT_mask_box_gesture(_MaskGesture, bpy.types.Operator):
    """Mask the vertices inside a screen-space box"""
    bl_idname = "sculptcore.mask_box_gesture"
    bl_label = "Box Mask"
    bl_options = {'REGISTER'}

    xmin: bpy.props.IntProperty(name="X Min", default=0)
    xmax: bpy.props.IntProperty(name="X Max", default=0)
    ymin: bpy.props.IntProperty(name="Y Min", default=0)
    ymax: bpy.props.IntProperty(name="Y Max", default=0)

    def _track(self, event):
        point = (event.mouse_region_x, event.mouse_region_y)
        if len(self._points) < 2:
            self._points.append(point)
        else:
            self._points[1] = point

    def _outline(self):
        if len(self._points) < 2:
            return self._points
        (x0, y0), (x1, y1) = self._points
        return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]

    def _apply(self, context):
        (x0, y0), (x1, y1) = self._points
        self.xmin, self.xmax = int(min(x0, x1)), int(max(x0, x1))
        self.ymin, self.ymax = int(min(y0, y1)), int(max(y0, y1))
        return self.execute(context) == {'FINISHED'}

    def execute(self, context):
        session = _session(context)
        if session is None or context.region_data is None:
            return {'CANCELLED'}
        xy, valid = _project_verts(context, session)
        inside = (valid
                  & (xy[:, 0] >= self.xmin) & (xy[:, 0] <= self.xmax)
                  & (xy[:, 1] >= self.ymin) & (xy[:, 1] <= self.ymax))
        if not _apply_mask(context, session, inside, self.mode, self.value,
                           "Box Mask"):
            return {'CANCELLED'}
        return {'FINISHED'}


class SCULPTCORE_OT_mask_lasso_gesture(_MaskGesture, bpy.types.Operator):
    """Mask the vertices inside a screen-space lasso"""
    bl_idname = "sculptcore.mask_lasso_gesture"
    bl_label = "Lasso Mask"
    bl_options = {'REGISTER'}

    def _track(self, event):
        point = (event.mouse_region_x, event.mouse_region_y)
        if not self._points or self._points[-1] != point:
            self._points.append(point)

    def _outline(self):
        return self._points

    def _apply(self, context):
        session = _session(context)
        if session is None:
            return False
        xy, valid = _project_verts(context, session)
        inside = points_in_polygon(xy, valid, self._points)
        return _apply_mask(context, session, inside, self.mode, self.value,
                           "Lasso Mask")


_classes = (
    SCULPTCORE_OT_mask_box_gesture,
    SCULPTCORE_OT_mask_lasso_gesture,
)


def register():
    for cls in _classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(_classes):
        bpy.utils.unregister_class(cls)
