# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Plane-mirror sculpt symmetry (Q4): the ``SymAxisMap`` reflection table and the
per-component sign flips the stroke operator applies to each mirror image.

Symmetry lives *above* the dab core (which stays mirror-agnostic): the operator
applies the primary dab, then one reflected dab per sign vector here. Each sign
vector flips a point or direction by component-wise multiply; the operator
reflects the resolved primary center and normal directly (mirror the operation,
as vanilla sculpt does) rather than re-raycasting the mirrored view ray — the
engine's ``castRay`` reconstructs hit positions too imprecisely for a re-cast to
be reflection-equivariant.

Only plane-mirror symmetry (X/Y/Z and their combinations) is modelled; radial
symmetry is deferred (no reference implementation).
"""

# Axis bitflags, matching Blender's mesh mirror flags.
AXIS_X = 1
AXIS_Y = 2
AXIS_Z = 4


def mirror_signs(sym):
    """The reflections implied by the axis bitmask ``sym`` as a list of
    ``(sx, sy, sz)`` sign tuples — one per non-empty subset of the enabled axes
    (X+Y+Z gives 7, covering every octant). Each component is -1 for an axis in
    the subset, else 1. The primary (unmirrored) image is not included, so an
    empty ``sym`` yields an empty list."""
    out = []
    for m in range(1, 8):
        if (m & sym) != m:
            continue  # a reflection may only use axes that are enabled
        out.append((
            -1.0 if (m & AXIS_X) else 1.0,
            -1.0 if (m & AXIS_Y) else 1.0,
            -1.0 if (m & AXIS_Z) else 1.0,
        ))
    return out


def axes_from_mesh(mesh):
    """The sculpt symmetry axis bitmask read from a Blender ``Mesh``
    (``use_mirror_x/y/z`` — the same flags vanilla sculpt mirrors across)."""
    sym = 0
    if getattr(mesh, "use_mirror_x", False):
        sym |= AXIS_X
    if getattr(mesh, "use_mirror_y", False):
        sym |= AXIS_Y
    if getattr(mesh, "use_mirror_z", False):
        sym |= AXIS_Z
    return sym


def reflect(vec, sign):
    """Component-wise multiply a 3-tuple ``vec`` by a sign tuple ``sign``
    (reflects a position or, since a sign flip preserves length, a direction /
    normal)."""
    return (vec[0] * sign[0], vec[1] * sign[1], vec[2] * sign[2])
