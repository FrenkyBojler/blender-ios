#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2024 Campbell Barton
#
# SPDX-License-Identifier: GPL-2.0-or-later

__all__ = (
    "main",
)


import json
import sys
import os

import bpy  # type: ignore
from bpy.types import (  # type: ignore
    Object,
)

BASE_DIR = os.path.abspath(os.path.dirname(__file__))

PREC_MAX = 6
OUTPUT_DIR = os.path.join(BASE_DIR, "data")


def clean_float(num: float, prec_max: int) -> float:
    """Round float to maximum precision, returning int if whole number."""
    num_rounded = round(num, prec_max)
    num_int = int(num_rounded)
    if num_int == num_rounded:
        return num_int
    return num_rounded


def signed_area(polygon: list[tuple[float, float]]) -> float:
    n = len(polygon)
    if n < 3:
        raise ValueError("A polygon must have at least 3 vertices.")

    area = 0.0
    for i in range(n):
        x1, y1 = polygon[i]
        x2, y2 = polygon[(i + 1) % n]  # wrap around
        area += (x1 * y2) - (x2 * y1)

    return area / 2.0


def export_object(ob: Object) -> None:
    import bmesh  # type: ignore

    bm = bmesh.new()
    me = ob.data
    bm.from_mesh(me)

    filename = ob.name + ".json"
    filepath = os.path.join(OUTPUT_DIR, filename)

    verts = list(bm.verts)
    edges = list(bm.edges)

    # Detect cyclic.
    is_cyclic_poly = True
    for v in verts:
        if len(v.link_edges) != 2:
            is_cyclic_poly = False
            break

    if is_cyclic_poly:
        edges.sort(
            key=lambda e: tuple(sorted([
                tuple(e.verts[0].co.xy),
                tuple(e.verts[1].co.xy),
            ]))
        )

        # Use a dict since it's ordered.
        edges_set = dict.fromkeys(edges)

        verts_ordered = []
        edges_ordered = []

        while edges_set:
            for e_init in edges_set:
                break
            verts_poly = []

            e = e_init
            if tuple(e.verts[0].co.xy) < tuple(e.verts[1].co.xy):
                v = e.verts[1]
            else:
                v = e.verts[0]

            e_next = None
            while True:
                del edges_set[e]
                verts_poly.append(v)
                if e.verts[0] == v:
                    v_next = e.verts[1]
                elif e.verts[1] == v:
                    v_next = e.verts[0]
                else:
                    raise RuntimeError("Invalid state for vert")
                e_next = None
                for e_other in v_next.link_edges:
                    if e_other is not e:
                        e_next = e_other
                        break
                if e_next is None:
                    raise RuntimeError("Invalid state for edge")
                if e_next is e_init:
                    break
                v = v_next
                e = e_next

            if (
                    len(verts_poly) >= 3 and
                    signed_area([v.co.xy for v in verts_poly]) < 0.0
            ):
                verts_poly.reverse()

            v_prev = verts_poly[-1]
            e_prev = None
            for v in verts_poly:
                v.index = len(verts_ordered)
                verts_ordered.append(v)
                e = bm.edges.get((v, v_prev))
                if e_prev is None:
                    e_prev = e
                else:
                    edges_ordered.append(e)
                v_prev = v
            edges_ordered.append(e_prev)

        assert len(verts_ordered) == len(bm.verts)
        assert set(verts_ordered) == set(bm.verts)
        assert len(edges_ordered) == len(bm.edges)
        assert set(edges_ordered) == set(bm.edges)

        verts = verts_ordered
        edges = edges_ordered

    # Build JSON data.
    verts_data = [
        [clean_float(v.co.x, prec_max=PREC_MAX), clean_float(v.co.y, prec_max=PREC_MAX)]
        for v in verts
    ]

    edges_data = []
    for e in edges:
        v_a, v_b = tuple(e.verts)
        i_a, i_b = v_a.index, v_b.index
        if is_cyclic_poly:
            if i_a > i_b:
                i_a, i_b = i_b, i_a
            # Wrapped around, reverse order so edge winding remains the same.
            if i_a + 1 != i_b:
                i_a, i_b = i_b, i_a
        edges_data.append([i_a, i_b])

    data = {
        "verts": verts_data,
        "edges": edges_data,
        "is_cyclic_poly": is_cyclic_poly,
    }

    with open(filepath, "w") as fh:
        json.dump(data, fh, indent=2)
        fh.write("\n")

    bm.free()


def main() -> int:
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    for ob in bpy.context.scene.objects:
        if ob.type != 'MESH':
            continue
        export_object(ob)
    return 0


if __name__ == "__main__":
    sys.exit(main())
