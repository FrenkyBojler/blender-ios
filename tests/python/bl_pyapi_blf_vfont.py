# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Tests for VFont (Text 3D object) rendering, comparing rasterized output
against reference images.

The text is created as a 3D text object, converted to mesh triangles,
and drawn into an offscreen buffer via the GPU module.

Usage:
  ./blender.bin --background --factory-startup --python tests/python/bl_pyapi_blf_vfont.py

To regenerate reference images:
  ./blender.bin --background --factory-startup --python tests/python/bl_pyapi_blf_vfont.py -- --generate

To generate an HTML report on failure:
  ./blender.bin --background --factory-startup --python tests/python/bl_pyapi_blf_vfont.py -- --show-html /tmp/vfont_report.html

To verify images are correctly framed (no clipping, no excessive margin):
  USE_TEST_BOUNDS_ERROR=1 ./blender.bin --background --factory-startup --python tests/python/bl_pyapi_blf_vfont.py
"""
__all__ = (
    "main",
)

import argparse
import contextlib
import os
import shutil
import subprocess
import sys
import tempfile
import unicodedata
import unittest

from typing import NamedTuple

import bpy  # type: ignore[import-not-found]
import imbuf  # type: ignore[import-not-found]
import mathutils  # type: ignore[import-not-found]
import gpu  # type: ignore[import-not-found]
import gpu.state  # type: ignore[import-not-found]
import gpu.matrix  # type: ignore[import-not-found]
from gpu_extras.batch import batch_for_shader  # type: ignore[import-not-found]

try:
    gpu.init()
except SystemError as ex:
    if os.environ.get("WITHOUT_GPU"):
        gpu = None
    else:
        sys.exit("GPU initialization failed: {:s}".format(str(ex)))


# ------------------------------------------------------------------------------
# Constants

FONT_NAME: str = "Inter.woff2"
FONT_SIZE: float = 1.0
# Render at 2x then scale down for basic anti-aliasing without heavy geometry.
SCALE_OVERSAMPLE: int = 2

SOURCE_DIR: str = os.path.abspath(os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..")))
TEST_DIR: str = os.path.join(SOURCE_DIR, "tests", "files", "blenfont")
RENDER_DIR: str = os.path.join(TEST_DIR, "vfont_renders")
IDIFF_BIN: str = os.environ.get("IDIFF_BIN") or shutil.which("idiff") or ""


# ------------------------------------------------------------------------------
# Globals

USE_GENERATE_TEST_DATA: bool = False
# When set, verify each rendered image is correctly framed:
# - No non-black pixels at the image border (content not clipped).
# - No edge with 13+ fully-black rows/columns (excessive unused margin).
# Enable via: `USE_TEST_BOUNDS_ERROR=1`.
USE_TEST_BOUNDS_ERROR: bool = bool(os.environ.get("USE_TEST_BOUNDS_ERROR"))
SHOW_HTML: str = ""
COMPARE_IMAGES: list["ComparedImage"] = []
OUTPUT_DIR: str = ""
FONT: "bpy.types.VectorFont | None" = None


# ------------------------------------------------------------------------------
# Types

class _StyleSpan(NamedTuple):
    bold: bool
    italic: bool
    underline: bool
    smallcaps: bool
    material: int
    kerning: float


class TextFormatCompose:
    """Builder for styled text, records per-character formatting for VFont.

    Example::

        TextFormatCompose().style(bold=True).text("hello").style(bold=False).text(" world")
    """

    def __init__(self) -> None:
        self._body: list[str] = []
        self._spans: list[_StyleSpan] = []
        self._bold: bool = False
        self._italic: bool = False
        self._underline: bool = False
        self._smallcaps: bool = False
        self._material: int = 0
        self._kerning: float = 0.0

    def style(
            self, *,
            bold: bool | None = None,
            italic: bool | None = None,
            underline: bool | None = None,
            smallcaps: bool | None = None,
            material: int | None = None,
            kerning: float | None = None,
    ) -> "TextFormatCompose":
        if bold is not None:
            self._bold = bold
        if italic is not None:
            self._italic = italic
        if underline is not None:
            self._underline = underline
        if smallcaps is not None:
            self._smallcaps = smallcaps
        if material is not None:
            self._material = material
        if kerning is not None:
            self._kerning = kerning
        return self

    def text(self, value: str) -> "TextFormatCompose":
        span = _StyleSpan(
            self._bold, self._italic, self._underline,
            self._smallcaps, self._material, self._kerning,
        )
        for ch in value:
            self._body.append(ch)
            self._spans.append(span)
        return self

    @property
    def body(self) -> str:
        return "".join(self._body)

    @property
    def spans(self) -> list[_StyleSpan]:
        return list(self._spans)


class ComparedImage(NamedTuple):
    group: str
    name: str
    text: str
    ref_path: str
    test_path: str
    passed: bool
    idiff_output: str


class TextBox(NamedTuple):
    """Mirror of :class:`bpy.types.TextBox` (every field a VFont text box exposes).

    Coordinates are in font units; ``y`` and ``height`` follow vfont's y-up convention,
    so a width-only box at the curve origin is ``TextBox(x=0, y=0, width=W, height=0)``.
    """
    x: float = 0.0
    y: float = 0.0
    width: float = 0.0
    height: float = 0.0


class RenderCase(NamedTuple):
    name: str
    text: str | TextFormatCompose
    size: tuple[int, int]
    expected_dimensions: tuple[float, float]
    position_offset: tuple[float, float] = (0.0, 0.0)
    font_size: float = FONT_SIZE
    text_boxes: list[TextBox] | None = None
    overflow: str = 'NONE'
    align_x: str = 'LEFT'
    align_y: str = 'TOP_BASELINE'
    space_word: float = 1.0
    materials: list[tuple[float, float, float]] | None = None


# ------------------------------------------------------------------------------
# Internal Utilities

_PHONETIC_ALPHABET: dict[str, str] = {
    "a": "alpha", "b": "bravo", "c": "charlie", "d": "delta",
    "e": "echo", "f": "foxtrot", "g": "golf", "h": "hotel",
    "i": "india", "j": "juliet", "k": "kilo", "l": "lima",
    "m": "mike", "n": "november", "o": "oscar", "p": "papa",
    "q": "quebec", "r": "romeo", "s": "sierra", "t": "tango",
    "u": "uniform", "v": "victor", "w": "whiskey", "x": "x-ray",
    "y": "yankee", "z": "zulu",
}


def generate_phonetic_alphabet(
        *,
        letters: tuple[str, str] = ("a", "z"),
        titlecase: bool = True,
) -> list[str]:
    """Return NATO phonetic alphabet words for a range of letters.

    Example: ``generate_phonetic_alphabet(letters=("a", "c"))`` returns
    ``["Alpha", "Bravo", "Charlie"]`` (with *titlecase=True*).
    """
    start = ord(letters[0].lower())
    end = ord(letters[1].lower())
    words = [_PHONETIC_ALPHABET[chr(c)] for c in range(start, end + 1)]
    if titlecase:
        words = [w.title() for w in words]
    return words


_PHONETIC_WORDS: list[str] = list(_PHONETIC_ALPHABET.values())


def generate_random_words(word_count: int, seed: int, *, titlecase: bool = True) -> list[str]:
    """Return *word_count* pseudo-random NATO phonetic words from *seed*.

    Uses a simple linear congruential generator so the output is
    deterministic and does not depend on :mod:`random`.
    """
    state = seed & 0xFFFFFFFF
    words: list[str] = []
    for _ in range(word_count):
        state = (state * 1103515245 + 12345) & 0xFFFFFFFF
        words.append(_PHONETIC_WORDS[state % len(_PHONETIC_WORDS)])
    if titlecase:
        words = [w.title() for w in words]
    return words


def load_font() -> "bpy.types.VectorFont":
    font_path = os.path.join(SOURCE_DIR, "release", "datafiles", "fonts", FONT_NAME)
    font = bpy.data.fonts.load(font_path)
    return font


def vfont_to_triangles(
        text: str | TextFormatCompose, font: "bpy.types.VectorFont", font_size: float,
        text_boxes: list[TextBox] | None = None,
        overflow: str = 'NONE',
        align_x: str = 'LEFT',
        align_y: str = 'TOP_BASELINE',
        space_word: float = 1.0,
        materials: list[tuple[float, float, float]] | None = None,
) -> tuple[list[tuple[float, float]], list[tuple[int, int, int]], list[int]]:
    """
    Create a VFont text object, convert to mesh, and extract 2D triangles.

    Returns ``(vertices, indices, material_indices)`` where vertices are ``(x, y)`` pairs,
    indices are triangle index triples, and material_indices is a per-triangle material slot.

    :param text: Plain string or :class:`TextFormatCompose` with per-character formatting.
    :param text_boxes: Optional list of :class:`TextBox` describing each text box region.
    :param overflow: Overflow mode: ``'NONE'``, ``'SCALE'``, or ``'TRUNCATE'``.
    :param materials: Optional list of ``(R, G, B)`` colors to assign as material slots.
    """
    styled = isinstance(text, TextFormatCompose)
    body = text.body if styled else text

    curve = bpy.data.curves.new(name="_test_text", type='FONT')
    curve.body = body
    curve.size = font_size
    curve.font = font
    curve.fill_mode = 'BOTH'
    # Just enough detail for visual comparison, keeps mesh small and tests fast.
    curve.resolution_u = 4
    curve.overflow = overflow
    curve.align_x = align_x
    curve.align_y = align_y
    curve.space_word = space_word

    if styled:
        for i, span in enumerate(text.spans):
            ci = curve.body_format[i]
            ci.use_bold = span.bold
            ci.use_italic = span.italic
            ci.use_underline = span.underline
            ci.use_small_caps = span.smallcaps
            ci.material_index = span.material
            ci.kerning = span.kerning

    created_materials: list["bpy.types.Material"] = []
    if materials is not None:
        for i, (r, g, b) in enumerate(materials):
            mat = bpy.data.materials.new(name="_test_mat_{:d}".format(i))
            mat.diffuse_color = (r, g, b, 1.0)
            curve.materials.append(mat)
            created_materials.append(mat)

    obj = bpy.data.objects.new(name="_test_text_obj", object_data=curve)
    bpy.context.collection.objects.link(obj)

    if text_boxes is not None:
        bpy.context.view_layer.objects.active = obj
        for i, tb_data in enumerate(text_boxes):
            if i >= len(curve.text_boxes):
                bpy.ops.font.textbox_add()
            tb = curve.text_boxes[i]
            tb.x = tb_data.x
            tb.y = tb_data.y
            tb.width = tb_data.width
            tb.height = tb_data.height

    bpy.context.view_layer.update()

    depsgraph = bpy.context.evaluated_depsgraph_get()
    eval_obj = obj.evaluated_get(depsgraph)
    mesh = eval_obj.to_mesh()
    mesh.calc_loop_triangles()

    verts = [(v.co.x, v.co.y) for v in mesh.vertices]
    tris = [(lt.vertices[0], lt.vertices[1], lt.vertices[2]) for lt in mesh.loop_triangles]
    tri_materials = [lt.material_index for lt in mesh.loop_triangles]

    eval_obj.to_mesh_clear()
    bpy.data.objects.remove(obj)
    bpy.data.curves.remove(curve)
    for mat in created_materials:
        bpy.data.materials.remove(mat)

    return verts, tris, tri_materials


def check_image_bounds(ibuf: object, name: str) -> str | None:
    """Check that the image is correctly framed.

    Returns an error message string, or ``None`` if the image is OK.

    - Error if any non-black pixel touches the image border.
    - Error if any border has 10 or more consecutive fully-black pixel rows/columns
      (too much unused margin).
    """
    w, h = ibuf.size
    with ibuf.with_buffer("BYTE") as pixels:
        data = bytes(pixels)

    def is_black(x: int, y: int) -> bool:
        off = (y * w + x) * 4
        return data[off] == 0 and data[off + 1] == 0 and data[off + 2] == 0

    # Check border pixels for non-black content.
    for x in range(w):
        if not is_black(x, 0):
            return "{:s}: non-black pixel at min_y edge (x={:d}), image=({:d}x{:d})".format(name, x, w, h)
        if not is_black(x, h - 1):
            return "{:s}: non-black pixel at max_y edge (x={:d}), image=({:d}x{:d})".format(name, x, w, h)
    for y in range(h):
        if not is_black(0, y):
            return "{:s}: non-black pixel at min_x edge (y={:d}), image=({:d}x{:d})".format(name, y, w, h)
        if not is_black(w - 1, y):
            return "{:s}: non-black pixel at max_x edge (y={:d}), image=({:d}x{:d})".format(name, y, w, h)

    # Count empty rows/columns at each edge.
    # Allow up to 12 pixels: 7 base margin + up to 5 from rounding size to nearest 10.
    margin_limit = 13

    margin_min_y = 0
    for y in range(h):
        if all(is_black(x, y) for x in range(w)):
            margin_min_y += 1
        else:
            break

    margin_max_y = 0
    for y in range(h - 1, -1, -1):
        if all(is_black(x, y) for x in range(w)):
            margin_max_y += 1
        else:
            break

    margin_min_x = 0
    for x in range(w):
        if all(is_black(x, y) for y in range(h)):
            margin_min_x += 1
        else:
            break

    margin_max_x = 0
    for x in range(w - 1, -1, -1):
        if all(is_black(x, y) for y in range(h)):
            margin_max_x += 1
        else:
            break

    for edge, margin in (
            ("min_x", margin_min_x),
            ("max_x", margin_max_x),
            ("min_y", margin_min_y),
            ("max_y", margin_max_y),
    ):
        if margin >= margin_limit:
            return (
                "{:s}: excessive margin at {:s}, "
                "{:d} empty pixels (limit {:d}), "
                "margins=(min_x={:d}, max_x={:d}, min_y={:d}, max_y={:d}), "
                "image=({:d}x{:d})"
            ).format(
                name, edge, margin, margin_limit,
                margin_min_x, margin_max_x, margin_min_y, margin_max_y,
                w, h,
            )

    return None


def vfont_dimensions_from_verts(
        verts: list[tuple[float, float]],
) -> tuple[float, float]:
    """Return the (width, height) from pre-computed triangle vertices."""
    if not verts:
        return (0.0, 0.0)
    xs = [v[0] for v in verts]
    ys = [v[1] for v in verts]
    return (max(xs) - min(xs), max(ys) - min(ys))


def render_text_vfont(
        verts: list[tuple[float, float]],
        tris: list[tuple[int, int, int]],
        tri_materials: list[int],
        image_size: tuple[int, int],
        position_offset: tuple[float, float],
        materials: list[tuple[float, float, float]] | None = None,
) -> object:
    """Render pre-computed VFont triangles into an imbuf via GPU.

    Renders at ``SCALE_OVERSAMPLE`` times the output resolution then scales down
    for basic anti-aliasing without increasing curve tessellation.
    """
    w, h = image_size
    s = SCALE_OVERSAMPLE
    sw, sh = w * s, h * s

    # Pixels-per-unit: font_size=1.0 maps to roughly 72 pixels tall.
    ppu = 72.0 * s
    # Transform vertices from VFont space to pixel space.
    ox, oy = position_offset[0] * s, position_offset[1] * s
    pixel_verts = [(v[0] * ppu + ox, v[1] * ppu + oy) for v in verts]

    offscreen = gpu.types.GPUOffScreen(sw, sh)
    with offscreen.bind():
        fb = gpu.state.active_framebuffer_get()
        fb.clear(color=(0.0, 0.0, 0.0, 1.0))
        gpu.state.blend_set("ALPHA")
        gpu.state.viewport_set(0, 0, sw, sh)
        gpu.matrix.load_matrix(mathutils.Matrix.Identity(4))
        gpu.matrix.load_projection_matrix(
            mathutils.Matrix(
                ((2 / sw, 0, 0, -1),
                 (0, 2 / sh, 0, -1),
                 (0, 0, -1, 0),
                 (0, 0, 0, 1)),
            )
        )

        if pixel_verts and tris:
            shader = gpu.shader.from_builtin('UNIFORM_COLOR')
            if materials is not None:
                # Draw each material group separately with its color.
                groups: dict[int, list[tuple[int, int, int]]] = {}
                for tri, mat_idx in zip(tris, tri_materials):
                    groups.setdefault(mat_idx, []).append(tri)
                for mat_idx, group_tris in groups.items():
                    r, g, b = materials[mat_idx] if mat_idx < len(materials) else (1.0, 1.0, 1.0)
                    batch = batch_for_shader(
                        shader, 'TRIS', {"pos": pixel_verts}, indices=group_tris,
                    )
                    shader.uniform_float("color", (r, g, b, 1.0))
                    batch.draw(shader)
            else:
                batch = batch_for_shader(shader, 'TRIS', {"pos": pixel_verts}, indices=tris)
                shader.uniform_float("color", (1.0, 1.0, 1.0, 1.0))
                batch.draw(shader)

        gpu.state.blend_set("NONE")

    pixel_buf = offscreen.texture_color.read()
    offscreen.free()

    # Read the oversampled buffer into an imbuf, then scale down.
    ibuf_hi = imbuf.new((sw, sh))
    ibuf_hi.file_type = "PNG"
    ibuf_hi.ensure_buffer("BYTE")
    gpu_bytes = bytearray(memoryview(pixel_buf).tobytes())
    expected_len = sw * sh * 4
    assert len(gpu_bytes) == expected_len, "GPU buffer {:d} != expected {:d}".format(len(gpu_bytes), expected_len)
    for i in range(3, len(gpu_bytes), 4):
        gpu_bytes[i] = 255
    with ibuf_hi.with_buffer("BYTE", write=True) as pixels:
        pixels[:] = gpu_bytes

    ibuf_hi.resize(image_size, method='BILINEAR')
    ibuf_hi.compress = 100
    return ibuf_hi


# ---------------------------------------------------------------------------
# HTML report (only used with --show-html)

def write_html_report(html_path: str) -> None:
    """Write an HTML report showing all compared images with idiff results."""
    import base64

    if not COMPARE_IMAGES:
        return

    def img_uri(path: str) -> str:
        if not os.path.exists(path):
            return ""
        with open(path, "rb") as fh_img:
            b64 = base64.b64encode(fh_img.read()).decode("ascii")
        return "data:image/png;base64,{:s}".format(b64)

    count_failed = 0
    with open(html_path, "w") as fh:
        fh.write(
            "<!DOCTYPE html>\n"
            "<html><head><meta charset='utf-8'>\n"
            "<title>VFont Test Report</title>\n"
            "</head><body bgcolor='#333' text='white'>\n"
            "<h1>VFont Test Report</h1>\n"
        )

        current_group = ""
        for entry in COMPARE_IMAGES:
            if entry.group != current_group:
                if current_group:
                    fh.write("</table>\n<hr>\n")
                fh.write("<h2>{:s}</h2>\n<table>\n".format(entry.group))
                current_group = entry.group
            if not entry.passed:
                count_failed += 1
                status = "<b><font color='red'>FAIL</font></b>"
            else:
                status = "<b><font color='green'>PASS</font></b>"

            diff_path = os.path.join(RENDER_DIR, "{:s}_diff.png".format(entry.name))
            if IDIFF_BIN and os.path.exists(entry.ref_path) and os.path.exists(entry.test_path):
                subprocess.run(
                    [IDIFF_BIN, "-fail", "0.004", "-failpercent", "0",
                     "-o", diff_path, "-abs", "-scale", "10",
                     entry.ref_path, entry.test_path],
                    capture_output=True, text=True,
                )

            idiff_stats = "\n".join(
                line for line in entry.idiff_output.splitlines()
                if not (line in {"PASS", "FAILURE"} or line.startswith("Comparing "))
            )
            fh.write(
                "<tr><td colspan='3'><h3>{name:s} {status:s}</h3>"
                "<code>{text:s}</code>"
                "<pre>{output:s}</pre></td></tr>\n"
                "<tr>\n"
                "  <td><small>Reference</small><br><img src='{ref:s}'></td>\n"
                "  <td><small>Test output</small><br><img src='{test:s}'></td>\n"
                "  <td><small>Diff (&times;10)</small><br><img src='{diff:s}'></td>\n"
                "</tr>\n".format(
                    name=entry.name,
                    status=status,
                    text=entry.text,
                    output=idiff_stats,
                    ref=img_uri(entry.ref_path),
                    test=img_uri(entry.test_path),
                    diff=img_uri(diff_path),
                )
            )

        count_total = len(COMPARE_IMAGES)
        fh.write(
            "</table>\n"
            "<p>{count_passed:d}/{count_total:d} passed</p>\n"
            "</body></html>\n".format(
                count_passed=count_total - count_failed,
                count_total=count_total,
            )
        )
    print("HTML report: {:s}".format(html_path))


# ---------------------------------------------------------------------------
# Test Mix-In

class TestImageComparison_MixIn:
    """
    Base class for tests that compare rendered images against references.

    Sub-classes must define ``name_prefix`` and ``cases`` class attributes,
    and inherit from both this class and :class:`unittest.TestCase`.
    """

    name_prefix: str = ""
    cases: list[RenderCase] = []

    def __init_subclass__(cls, **kwargs: object) -> None:
        super().__init_subclass__(**kwargs)
        if "cases" not in cls.__dict__:
            raise TypeError("{:s} must define 'cases'".format(cls.__name__))
        if "name_prefix" not in cls.__dict__:
            raise TypeError("{:s} must define 'name_prefix'".format(cls.__name__))

    def setUp(self) -> None:
        assert isinstance(self, unittest.TestCase)
        self.font = FONT

    def test_cases(self) -> None:
        """Check dimensions and rendered image for each case."""
        assert isinstance(self, unittest.TestCase)
        for case in self.cases:
            with self.subTest(name=case.name):
                verts, tris, tri_materials = vfont_to_triangles(
                    case.text, self.font, case.font_size,
                    case.text_boxes, case.overflow,
                    case.align_x, case.align_y, case.space_word,
                    case.materials,
                )

                w, h = vfont_dimensions_from_verts(verts)
                self.assertAlmostEqual(
                    w, case.expected_dimensions[0], places=4,
                    msg="Width mismatch for {:s}: got {:.4f} expected {:.4f}".format(
                        case.name, w, case.expected_dimensions[0],
                    ),
                )
                self.assertAlmostEqual(
                    h, case.expected_dimensions[1], places=4,
                    msg="Height mismatch for {:s}: got {:.4f} expected {:.4f}".format(
                        case.name, h, case.expected_dimensions[1],
                    ),
                )

                ibuf = render_text_vfont(
                    verts, tris, tri_materials,
                    case.size, case.position_offset, case.materials,
                )

                if USE_TEST_BOUNDS_ERROR:
                    bounds_err = check_image_bounds(ibuf, case.name)
                    self.assertIsNone(bounds_err, bounds_err)

                text_display = case.text.body if isinstance(case.text, TextFormatCompose) else case.text
                self._compare_image(case.name, text_display, ibuf)

    def _compare_image(self, name: str, text: str, ibuf: object) -> None:
        """Compare rendered image against a reference using idiff, or generate if --generate."""
        assert isinstance(self, unittest.TestCase)
        name_full = self.name_prefix + name
        ref_path = os.path.join(RENDER_DIR, "{:s}.png".format(name_full))
        out_path = os.path.join(OUTPUT_DIR, "{:s}_test.png".format(name_full))

        if USE_GENERATE_TEST_DATA:
            imbuf.write(ibuf, filepath=ref_path)
            print("  Generated: {:s}".format(ref_path))
            return

        self.assertTrue(os.path.exists(ref_path), "Reference image missing: {:s}".format(ref_path))

        imbuf.write(ibuf, filepath=out_path)

        self.assertTrue(IDIFF_BIN, "idiff not found, set IDIFF_BIN or install OpenImageIO")

        result = subprocess.run(
            [IDIFF_BIN, "-fail", "0.004", "-failpercent", "0", ref_path, out_path],
            capture_output=True,
            text=True,
        )

        passed = result.returncode == 0
        COMPARE_IMAGES.append(ComparedImage(
            group=type(self).__name__,
            name=name_full,
            text=text,
            ref_path=ref_path,
            test_path=out_path,
            passed=passed,
            idiff_output=result.stdout.rstrip(),
        ))

        self.assertEqual(
            result.returncode, 0,
            "Image {:s} differs from reference:\n{:s}".format(name_full, result.stdout),
        )


# ---------------------------------------------------------------------------
# Tests

class TestVFontBasic(TestImageComparison_MixIn, unittest.TestCase):
    """Basic VFont text rendering."""

    name_prefix = "basic."
    cases = [
        RenderCase(
            name="alpha_bravo",
            text=" ".join(generate_phonetic_alphabet(letters=("a", "b"))),
            size=(300, 70),
            expected_dimensions=(3.938546, 0.651417),
            position_offset=(6.8, 21.3),
        ),
        RenderCase(
            name="charlie_delta",
            text=" ".join(generate_phonetic_alphabet(letters=("c", "e"))),
            size=(450, 60),
            expected_dimensions=(5.980540, 0.532264),
            position_offset=(6.1, 10.6),
        ),
        RenderCase(
            name="combining_diaeresis",
            text="X\u0308V",
            size=(90, 60),
            expected_dimensions=(0.919085, 0.523387),
            position_offset=(9.7, 11.0),
        ),
        RenderCase(
            name="combining_nfd",
            text=unicodedata.normalize("NFD", "\u00C8ve"),
            size=(100, 60),
            expected_dimensions=(1.118812, 0.537385),
            position_offset=(4.7, 10.6),
        ),
    ]


class TestVFontCombiningKerning(TestImageComparison_MixIn, unittest.TestCase):
    """Test that kerning is correct after combining characters."""

    name_prefix = "combining_kerning."
    cases = [
        RenderCase(
            name="xv",
            text="X\u0308V",
            size=(90, 60),
            expected_dimensions=(0.919085, 0.523387),
            position_offset=(9.7, 11.0),
        ),
        RenderCase(
            name="ro",
            text="r\u0308o",
            size=(60, 60),
            expected_dimensions=(0.622909, 0.531581),
            position_offset=(5.8, 10.6),
        ),
        RenderCase(
            name="Yolanda",
            text=unicodedata.normalize("NFD", "\u0178olanda"),
            size=(210, 60),
            expected_dimensions=(2.619665, 0.531922),
            position_offset=(8.8, 10.6),
        ),
        RenderCase(
            name="Avatar",
            text=unicodedata.normalize("NFD", "\u00C0vatar"),
            size=(170, 60),
            expected_dimensions=(2.101741, 0.537726),
            position_offset=(7.8, 10.6),
        ),
        RenderCase(
            name="Eve",
            text=unicodedata.normalize("NFD", "\u00C8ve"),
            size=(100, 60),
            expected_dimensions=(1.118812, 0.537385),
            position_offset=(4.7, 10.6),
        ),
    ]


class TestVFontCombiningPosition(TestImageComparison_MixIn, unittest.TestCase):
    """Test that combining marks are visually positioned over their base character."""

    name_prefix = "combining_position."
    cases = [
        RenderCase(
            name="diaeresis_Y",
            text="Y\u0308",
            size=(50, 60),
            expected_dimensions=(0.438716, 0.523387),
            position_offset=(7.8, 11.0),
        ),
        RenderCase(
            name="grave_A",
            text="A\u0300",
            size=(50, 60),
            expected_dimensions=(0.446569, 0.529191),
            position_offset=(6.8, 10.0),
        ),
        RenderCase(
            name="acute_e",
            text="e\u0301",
            size=(40, 60),
            expected_dimensions=(0.336975, 0.537385),
            position_offset=(4.5, 10.6),
        ),
        RenderCase(
            name="phrase",
            text=unicodedata.normalize("NFD", "\u00C0vatar \u00C8ve \u0178olanda"),
            size=(480, 60),
            expected_dimensions=(6.340730, 0.537726),
            position_offset=(9.8, 10.6),
        ),
    ]


class TestVFontCombiningEdgePositions(TestImageComparison_MixIn, unittest.TestCase):
    """Test combining marks at edge positions: start, end, after space/zero-width."""

    name_prefix = "combining_edge."
    cases = [
        # Combining mark as first character (no base).
        RenderCase(
            name="combining_first",
            text="\u0308Hello",
            size=(150, 60),
            expected_dimensions=(1.792421, 0.531581),
            position_offset=(18.1, 10.6),
        ),
        # Combining mark after a space.
        RenderCase(
            name="combining_after_space",
            text="A \u0308B",
            size=(100, 60),
            expected_dimensions=(1.068966, 0.523387),
            position_offset=(9.8, 11.0),
        ),
        # Combining mark at end of string.
        RenderCase(
            name="combining_at_end",
            text="Hello\u0308",
            size=(140, 60),
            expected_dimensions=(1.620007, 0.531581),
            position_offset=(6.7, 10.6),
        ),
        # Many combining marks with no base character.
        RenderCase(
            name="many_no_base",
            text="\u0308\u0301\u0300\u0302\u0303\u0304\u0305\u0306\u0307\u0309",
            size=(50, 60),
            expected_dimensions=(0.485336, 0.552321),
            position_offset=(15.1, 9.0),
        ),
    ]


class TestCombiningSequences(TestImageComparison_MixIn, unittest.TestCase):
    """Test combining mark sequences: bursts, alternating, repeated."""

    name_prefix = "combining_sequences."
    cases = [
        RenderCase(
            name="base_then_burst",
            text="ABCDE\u0308\u0301\u0300",
            size=(190, 60),
            expected_dimensions=(2.306931, 0.536019),
            position_offset=(9.8, 10.5),
        ),
        RenderCase(
            name="alternating",
            text="a\u0308b\u0308c\u0308d\u0308",
            size=(130, 60),
            expected_dimensions=(1.550700, 0.531922),
            position_offset=(6.8, 10.6),
        ),
        RenderCase(
            name="repeated_mark",
            text="A\u0308\u0308\u0308",
            size=(50, 60),
            expected_dimensions=(0.446569, 0.523387),
            position_offset=(6.8, 11.0),
        ),
    ]


class TestComplexScripts(TestImageComparison_MixIn, unittest.TestCase):
    """Test complex scripts with combining marks."""

    name_prefix = "complex_scripts."
    cases = [
        RenderCase(
            name="arabic",
            text="\u0628\u064e\u0633\u0650\u0645\u064f \u0627\u0644\u0644\u0651\u0647\u0650",
            size=(390, 60),
            expected_dimensions=(5.192762, 0.552321),
            position_offset=(5.5, 9.0),
        ),
        RenderCase(
            name="devanagari",
            text="\u0928\u092e\u0938\u094d\u0924\u0947",
            size=(200, 60),
            expected_dimensions=(2.466917, 0.552321),
            position_offset=(9.5, 9.0),
        ),
        RenderCase(
            name="thai",
            text="\u0e2a\u0e27\u0e31\u0e2a\u0e14\u0e35\u0e04\u0e23\u0e31\u0e1a",
            size=(320, 60),
            expected_dimensions=(4.158962, 0.552321),
            position_offset=(8.5, 9.0),
        ),
    ]


class TestStressCombining(TestImageComparison_MixIn, unittest.TestCase):
    """Stress tests for combining mark handling."""

    name_prefix = "stress_combining."
    cases = [
        # Many combining marks on one base.
        RenderCase(
            name="all_diacriticals",
            text="X" + "".join(chr(c) for c in range(0x0300, 0x0370)),
            size=(1470, 80),
            expected_dimensions=(20.195456, 0.913622),
            position_offset=(5.7, 23.2),
        ),
        # Extreme stacking depth.
        RenderCase(
            name="zalgo_100",
            text="H" + ("\u0308\u0301\u0300\u0302\u0303" * 20) + "ello",
            size=(140, 60),
            expected_dimensions=(1.620007, 0.537385),
            position_offset=(6.7, 10.6),
        ),
        # Zero-width joiner between combining marks.
        RenderCase(
            name="cgj",
            text="A\u0308\u034f\u0301B",
            size=(80, 60),
            expected_dimensions=(0.881188, 0.529191),
            position_offset=(6.8, 10.0),
        ),
    ]


class TestStrangeCharacters(TestImageComparison_MixIn, unittest.TestCase):
    """Test that control characters, zero-width characters, and edge cases don't crash."""

    name_prefix = "strange_characters."
    cases = [
        RenderCase(
            name="tab",
            text="A\tB",
            size=(197, 60),
            expected_dimensions=(2.4015, 0.508706),
            position_offset=(10.0, 11.0),
        ),
        RenderCase(
            name="carriage_return",
            text="A\rB",
            size=(100, 60),
            expected_dimensions=(1.0690, 0.508706),
            position_offset=(10.0, 11.0),
        ),
        RenderCase(
            name="line_feed",
            text="A\nB",
            size=(56, 133),
            expected_dimensions=(0.4466, 1.508706),
            position_offset=(10.0, 84.0),
        ),
        RenderCase(
            name="cr_lf",
            text="A\r\nB",
            size=(56, 133),
            expected_dimensions=(0.4466, 1.508706),
            position_offset=(10.0, 84.0),
        ),
        RenderCase(
            name="bom",
            text="A\ufeffB",
            size=(87, 60),
            expected_dimensions=(0.8812, 0.508706),
            position_offset=(10.0, 11.0),
        ),
        RenderCase(
            name="zero_width_joiner",
            text="A\u200dB",
            size=(118, 60),
            expected_dimensions=(1.3042, 0.552321),
            position_offset=(10.0, 11.0),
        ),
        RenderCase(
            name="replacement_char",
            text="A\ufffdB",
            size=(118, 60),
            expected_dimensions=(1.3042, 0.552321),
            position_offset=(10.0, 11.0),
        ),
        RenderCase(
            name="empty_string",
            text="",
            size=(12, 12),
            expected_dimensions=(0.0, 0.0),
            position_offset=(5.0, 5.0),
        ),
        RenderCase(
            name="only_combining",
            text="\u0308\u0301",
            size=(12, 12),
            expected_dimensions=(0.2247, 0.100376),
            position_offset=(5.0, 5.0),
        ),
        RenderCase(
            name="double_combining",
            text="U\u0308\u0301B",
            size=(88, 60),
            expected_dimensions=(0.8750, 0.537385),
            position_offset=(8.0, 11.0),
        ),
        # Every code-point in 1..255 forwards & backwards as a stress test.
        RenderCase(
            name="ascii_sweep",
            text="".join((
                *(chr(c) for c in range(1, 256)),
                "\n\n",
                *(chr(c) for c in range(255, 0, -1)),
            )),
            size=(360, 24),
            expected_dimensions=(4.769871, 0.143052),
            font_size=FONT_SIZE / 20.0,
            position_offset=(8.0, 16.0),
        ),
    ]


class TestStressZeroWidth(TestImageComparison_MixIn, unittest.TestCase):
    """Stress tests for zero-width and invisible characters."""

    name_prefix = "stress_zero_width."
    cases = [
        # Variation selectors between base characters.
        RenderCase(
            name="var_selector",
            text="A\ufe00B\ufe01C",
            size=(185, 60),
            expected_dimensions=(2.2383, 0.559150),
            position_offset=(10.0, 11.0),
        ),
        # BOM at start of string (should be invisible).
        RenderCase(
            name="bom_start",
            text="\ufeffHello",
            size=(140, 60),
            expected_dimensions=(1.6200, 0.516900),
            position_offset=(8.0, 11.0),
        ),
        # Word joiner preventing line break.
        RenderCase(
            name="word_joiner",
            text="Hello\u2060World",
            size=(313, 60),
            expected_dimensions=(3.9986, 0.560515),
            position_offset=(8.0, 11.0),
        ),
        # Soft hyphen inside a word.
        RenderCase(
            name="soft_hyphen",
            text="break\u00adable",
            size=(284, 60),
            expected_dimensions=(3.6016, 0.560857),
            position_offset=(8.0, 11.0),
        ),
        # Multiple zero-width chars in sequence.
        RenderCase(
            name="sequence",
            text="A\u200b\u200c\u200d\u200eB",
            size=(179, 60),
            expected_dimensions=(2.1502, 0.552321),
            position_offset=(10.0, 11.0),
        ),
    ]


class TestStressBidi(TestImageComparison_MixIn, unittest.TestCase):
    """Stress tests for bidirectional and layout control characters."""

    name_prefix = "stress_bidi."
    cases = [
        # Left-to-right and right-to-left marks between base chars.
        RenderCase(
            name="lrm_rlm",
            text="A\u200eB\u200fC",
            size=(185, 60),
            expected_dimensions=(2.2383, 0.559150),
            position_offset=(10.0, 11.0),
        ),
        # RTL override reversing Latin text.
        RenderCase(
            name="rtl_override",
            text="\u202eHello\u202c",
            size=(203, 60),
            expected_dimensions=(2.4891, 0.560515),
            position_offset=(10.0, 11.0),
        ),
        # Nested LTR/RTL embeddings.
        RenderCase(
            name="nested_embed",
            text="\u202aHello \u202bWorld\u202c\u202c",
            size=(420, 60),
            expected_dimensions=(5.4970, 0.560515),
            position_offset=(10.0, 11.0),
        ),
        # Arabic and Latin mixed with explicit bidi marks.
        RenderCase(
            name="mixed_script",
            text="Hello \u200f\u0645\u0631\u062d\u0628\u0627\u200e World",
            size=(523, 60),
            expected_dimensions=(6.9123, 0.560515),
            position_offset=(8.0, 11.0),
        ),
    ]


class TestStressWidths(TestImageComparison_MixIn, unittest.TestCase):
    """Stress tests for wide and variable-width characters."""

    name_prefix = "stress_widths."
    cases = [
        # Double-width CJK ideographs.
        RenderCase(
            name="cjk",
            text="\u4e16\u754c\u4f60\u597d",
            size=(140, 60),
            expected_dimensions=(1.6209, 0.552321),
            position_offset=(10.0, 11.0),
        ),
        # Fullwidth Latin letters (double advance).
        RenderCase(
            name="fullwidth_latin",
            text="\uff21\uff22\uff23",
            size=(110, 60),
            expected_dimensions=(1.1979, 0.552321),
            position_offset=(10.0, 11.0),
        ),
        # Halfwidth Katakana (single advance).
        RenderCase(
            name="halfwidth_kana",
            text="\uff76\uff77\uff78",
            size=(110, 60),
            expected_dimensions=(1.1979, 0.552321),
            position_offset=(10.0, 11.0),
        ),
        # Fullwidth space between CJK.
        RenderCase(
            name="fullwidth_space",
            text="\u4e16\u3000\u754c",
            size=(110, 60),
            expected_dimensions=(1.1979, 0.552321),
            position_offset=(10.0, 11.0),
        ),
        # CJK with combining mark followed by Latin.
        RenderCase(
            name="mixed",
            text="\u4e16\u0308Hello",
            size=(174, 60),
            expected_dimensions=(2.0820, 0.560515),
            position_offset=(10.0, 11.0),
        ),
        # Figure, thin, and hair spaces between Latin.
        RenderCase(
            name="unicode_spaces",
            text="A\u2007B\u2009C\u200aD",
            size=(206, 60),
            expected_dimensions=(2.5299, 0.522363),
            position_offset=(10.0, 11.0),
        ),
    ]


class TestStressInvalidCodepoints(TestImageComparison_MixIn, unittest.TestCase):
    """Stress tests for invalid and undefined codepoints."""

    name_prefix = "stress_invalid_codepoints."
    cases = [
        # Noncharacters (permanently undefined, must not crash).
        RenderCase(
            name="nonchar_fdd0",
            text="A\ufdd0B",
            size=(118, 60),
            expected_dimensions=(1.3042, 0.552321),
            position_offset=(10.0, 11.0),
        ),
        RenderCase(
            name="nonchar_ffff",
            text="A\uffffB",
            size=(118, 60),
            expected_dimensions=(1.3042, 0.552321),
            position_offset=(10.0, 11.0),
        ),
        # Specials block including unassigned codepoints.
        RenderCase(
            name="specials",
            text="\ufff0\ufff1\ufffd",
            size=(110, 60),
            expected_dimensions=(1.1979, 0.552321),
            position_offset=(10.0, 11.0),
        ),
        # Replacement character repeated.
        RenderCase(
            name="replacement",
            text="\ufffd\ufffd\ufffd",
            size=(110, 60),
            expected_dimensions=(1.1979, 0.552321),
            position_offset=(10.0, 11.0),
        ),
        # Maximum valid codepoint U+10FFFF.
        RenderCase(
            name="max_codepoint",
            text="A\U0010ffffB",
            size=(118, 60),
            expected_dimensions=(1.3042, 0.552321),
            position_offset=(10.0, 11.0),
        ),
    ]


class TestTextBoxes(TestImageComparison_MixIn, unittest.TestCase):
    """Test text layout with text boxes of various configurations."""

    name_prefix = "text_boxes."
    cases = [
        # Single narrow text box, forces word wrap.
        RenderCase(
            name="narrow_wrap",
            text=" ".join(generate_random_words(3, seed=10)),
            size=(260, 210),
            expected_dimensions=(3.374872, 2.651417),
            position_offset=(5.2, 163.3),
            text_boxes=[TextBox(0.0, 0.0, 1.5, 0.0)],
        ),
        # Tall box with limited height, text should be clipped.
        RenderCase(
            name="height_limit",
            text="\n".join(generate_random_words(4, seed=20)),
            size=(190, 270),
            expected_dimensions=(2.410038, 3.532264),
            position_offset=(6.8, 223.6),
            text_boxes=[TextBox(0.0, 0.0, 4.0, 2.0)],
        ),
        # Two text boxes side by side, text flows from first to second.
        RenderCase(
            name="two_boxes",
            text=" ".join(generate_random_words(8, seed=30)),
            size=(430, 500),
            expected_dimensions=(5.723797, 6.653807),
            position_offset=(3.7, 452.4),
            text_boxes=[
                TextBox(0.0, 0.0, 2.5, 1.5),
                TextBox(3.0, 0.0, 2.5, 1.5),
            ],
        ),
        # Offset text box (non-zero x, y origin).
        RenderCase(
            name="offset_origin",
            text=" ".join(generate_random_words(1, seed=40)),
            size=(170, 70),
            expected_dimensions=(2.118812, 0.653807),
            position_offset=(-65.3, 93.4),
            text_boxes=[TextBox(1.0, -1.0, 3.0, 0.0)],
        ),
        # Very narrow box with combining characters.
        RenderCase(
            name="narrow_combining",
            text=unicodedata.normalize("NFD", "\u00C0vatar \u00C8ve"),
            size=(170, 130),
            expected_dimensions=(2.101741, 1.537385),
            position_offset=(7.8, 81.6),
            text_boxes=[TextBox(0.0, 0.0, 1.0, 0.0)],
        ),
    ]


class TestVFontGridLayouts(TestImageComparison_MixIn, unittest.TestCase):
    """4x4 grid of text boxes that text flows through, with non-standard word spacing
    and a very small font. Exercises multi-text-box layout in a single curve.
    """

    name_prefix = "grid_layouts."
    cases = [
        RenderCase(
            name="grid_4x4",
            # Enough words to overflow through several boxes without making the
            # curve-fill triangulation pass slow on the test build.
            text=" ".join(generate_random_words(40, seed=42)),
            text_boxes=[
                TextBox(col * 1.6, -row * 1.0, 1.6, 1.0)
                for row in range(4)
                for col in range(4)
            ],
            align_x='JUSTIFY',
            align_y='TOP',
            space_word=1.25,
            font_size=FONT_SIZE / 20.0,
            size=(138, 36),
            expected_dimensions=(1.598464, 0.176596),
            position_offset=(10.0, 22.0),
        ),
    ]


class TestStyles(TestImageComparison_MixIn, unittest.TestCase):
    """Test per-character styling: bold, italic, underline, smallcaps."""

    name_prefix = "styles."
    cases = [
        RenderCase(
            name="bold_word",
            text=(
                TextFormatCompose()
                .style(bold=True).text("Alpha")
                .style(bold=False).text(" Bravo")
            ),
            size=(330, 90),
            expected_dimensions=(4.375479, 0.916000),
            position_offset=(7.0, 27.6),
        ),
        RenderCase(
            name="italic_word",
            text=(
                TextFormatCompose()
                .text("Charlie ")
                .style(italic=True).text("Delta")
            ),
            size=(360, 70),
            expected_dimensions=(4.667589, 0.691000),
            position_offset=(8.1, 9.6),
        ),
        RenderCase(
            name="bold_italic",
            text=(
                TextFormatCompose()
                .style(bold=True, italic=True).text("Echo")
                .style(bold=False, italic=False).text(" Foxtrot")
            ),
            size=(340, 70),
            expected_dimensions=(4.421316, 0.691000),
            position_offset=(4.6, 9.6),
        ),
        RenderCase(
            name="underline",
            text=(
                TextFormatCompose()
                .text("Golf ")
                .style(underline=True).text("Hotel")
                .style(underline=False).text(" India")
            ),
            size=(390, 60),
            expected_dimensions=(5.103790, 0.629874),
            position_offset=(8.1, 14.2),
        ),
        RenderCase(
            name="smallcaps",
            text=(
                TextFormatCompose()
                .text("Juliet ")
                .style(smallcaps=True).text("Kilo")
                .style(smallcaps=False).text(" Lima")
            ),
            size=(380, 60),
            expected_dimensions=(5.028594, 0.532264),
            position_offset=(5.6, 10.6),
        ),
        RenderCase(
            name="mixed",
            text=(
                TextFormatCompose()
                .style(bold=True).text("Mike")
                .style(bold=False).text(" ")
                .style(italic=True).text("November")
                .style(italic=False).text(" ")
                .style(underline=True).text("Oscar")
                .style(underline=False).text(" ")
                .style(smallcaps=True).text("Papa")
            ),
            size=(760, 80),
            expected_dimensions=(10.271526, 0.782000),
            position_offset=(4.7, 18.2),
        ),
        # Tight kerning.
        RenderCase(
            name="kern_tight",
            text=(
                TextFormatCompose()
                .style(kerning=-5.0).text("AV")
                .style(kerning=0.0).text("atar")
            ),
            size=(170, 60),
            expected_dimensions=(2.072294, 0.517241),
            position_offset=(8.8, 11.6),
        ),
        # Wide kerning.
        RenderCase(
            name="kern_wide",
            text=(
                TextFormatCompose()
                .style(kerning=10.0).text("Hello")
            ),
            size=(160, 60),
            expected_dimensions=(1.944776, 0.516900),
            position_offset=(4.7, 11.6),
        ),
    ]


class TestMaterials(TestImageComparison_MixIn, unittest.TestCase):
    """Test per-character material assignment with colored rendering."""

    name_prefix = "materials."
    cases = [
        # Two materials: red and blue.
        RenderCase(
            name="two_colors",
            text=(
                TextFormatCompose()
                .style(material=0).text("Red")
                .text(" ")
                .style(material=1).text("Blue")
            ),
            size=(220, 60),
            expected_dimensions=(2.831342, 0.516900),
            position_offset=(2.7, 11.6),
            materials=[(1.0, 0.2, 0.2), (0.2, 0.4, 1.0)],
        ),
        # Three materials: red, green, blue.
        RenderCase(
            name="three_colors",
            text=(
                TextFormatCompose()
                .style(material=0).text("Red ")
                .style(material=1).text("Green ")
                .style(material=2).text("Blue")
            ),
            size=(380, 60),
            expected_dimensions=(5.016047, 0.523728),
            position_offset=(4.7, 11.6),
            materials=[(1.0, 0.2, 0.2), (0.2, 1.0, 0.2), (0.2, 0.4, 1.0)],
        ),
        # Material with bold styling.
        RenderCase(
            name="colored_bold",
            text=(
                TextFormatCompose()
                .style(material=0, bold=True).text("Yellow")
                .style(material=1, bold=False).text(" Cyan")
            ),
            size=(350, 80),
            expected_dimensions=(4.625391, 0.827101),
            position_offset=(8.0, 20.4),
            materials=[(1.0, 0.8, 0.2), (0.2, 0.8, 1.0)],
        ),
        # CMYK: each letter underlined and colored with its corresponding material slot.
        # K (Key) is a dark gray so it stays visible against the black background.
        RenderCase(
            name="cmyk",
            text=(
                TextFormatCompose()
                .style(material=0, underline=True).text("C")
                .style(material=1, underline=True).text("M")
                .style(material=2, underline=True).text("Y")
                .style(material=3, underline=True).text("K")
            ),
            size=(172, 69),
            expected_dimensions=(2.076818, 0.615534),
            position_offset=(10.0, 19.0),
            materials=[
                (0.0, 1.0, 1.0),  # C - Cyan
                (1.0, 0.0, 1.0),  # M - Magenta
                (1.0, 1.0, 0.0),  # Y - Yellow
                (0.4, 0.4, 0.4),  # K - Key (dark gray)
            ],
        ),
    ]


class TestSizeAndSpacing(TestImageComparison_MixIn, unittest.TestCase):
    """Test font size and word spacing variations."""

    name_prefix = "size_spacing."
    cases = [
        # Half size.
        RenderCase(
            name="size_half",
            text=" ".join(generate_phonetic_alphabet(letters=("a", "b"))),
            size=(160, 40),
            expected_dimensions=(1.969273, 0.325708),
            position_offset=(7.4, 13.1),
            font_size=0.5,
        ),
        # Double size.
        RenderCase(
            name="size_double",
            text=" ".join(generate_phonetic_alphabet(letters=("a", "b"))),
            size=(590, 110),
            expected_dimensions=(7.877092, 1.302834),
            position_offset=(8.6, 27.6),
            font_size=2.0,
        ),
        # Tight word spacing.
        RenderCase(
            name="word_spacing_tight",
            text=" ".join(generate_phonetic_alphabet(letters=("e", "h"))),
            size=(540, 60),
            expected_dimensions=(7.175316, 0.538068),
            position_offset=(6.7, 10.6),
            space_word=0.25,
        ),
        # Wide word spacing.
        RenderCase(
            name="word_spacing_wide",
            text=" ".join(generate_phonetic_alphabet(letters=("e", "h"))),
            size=(600, 60),
            expected_dimensions=(8.020315, 0.538068),
            position_offset=(6.7, 10.6),
            space_word=1.75,
        ),
    ]


class TestOverflow(TestImageComparison_MixIn, unittest.TestCase):
    """Test overflow mode (text spills outside the text box)."""

    name_prefix = "overflow."
    cases = [
        # Text overflows a small box.
        RenderCase(
            name="spill_single",
            text=" ".join(generate_random_words(4, seed=50)),
            size=(180, 270),
            expected_dimensions=(2.264254, 3.517241),
            position_offset=(5.8, 224.6),
            text_boxes=[TextBox(0.0, 0.0, 2.0, 1.0)],
            overflow='NONE',
        ),
        # Multiline overflow past box height.
        RenderCase(
            name="spill_multiline",
            text="\n".join(generate_random_words(5, seed=60)),
            size=(260, 350),
            expected_dimensions=(3.372823, 4.531922),
            position_offset=(5.1, 299.6),
            text_boxes=[TextBox(0.0, 0.0, 3.0, 1.5)],
            overflow='NONE',
        ),
        # Combining characters overflow.
        RenderCase(
            name="spill_combining",
            text=unicodedata.normalize("NFD", "\u00C0vatar \u00C8ve \u0178olanda"),
            size=(210, 200),
            expected_dimensions=(2.619665, 2.537726),
            position_offset=(8.8, 152.6),
            text_boxes=[TextBox(0.0, 0.0, 2.0, 1.0)],
            overflow='NONE',
        ),
        # Alignment variations.
        RenderCase(
            name="align_left",
            text="\n".join(generate_random_words(4, seed=70)),
            size=(200, 270),
            expected_dimensions=(2.538068, 3.517241),
            position_offset=(5.1, 224.6),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 0.0)],
            overflow='NONE',
            align_x='LEFT',
        ),
        RenderCase(
            name="align_center",
            text="\n".join(generate_random_words(4, seed=70)),
            size=(200, 270),
            expected_dimensions=(2.518266, 3.517241),
            position_offset=(9.4, 224.6),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 0.0)],
            overflow='NONE',
            align_x='CENTER',
        ),
        RenderCase(
            name="align_right",
            text="\n".join(generate_random_words(4, seed=70)),
            size=(200, 270),
            expected_dimensions=(2.562649, 3.517241),
            position_offset=(12.1, 224.6),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 0.0)],
            overflow='NONE',
            align_x='RIGHT',
        ),
        RenderCase(
            name="align_justify",
            text="\n".join(generate_random_words(4, seed=70)),
            size=(200, 270),
            expected_dimensions=(2.538068, 3.517241),
            position_offset=(5.1, 224.6),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 0.0)],
            overflow='NONE',
            align_x='JUSTIFY',
        ),
        RenderCase(
            name="align_flush",
            text="\n".join(generate_random_words(4, seed=70)),
            size=(200, 270),
            expected_dimensions=(2.451178, 3.517241),
            position_offset=(8.1, 224.6),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 0.0)],
            overflow='NONE',
            align_x='FLUSH',
        ),
        # Vertical alignment variations.
        RenderCase(
            name="valign_top",
            text="\n".join(generate_random_words(4, seed=70)),
            size=(200, 270),
            expected_dimensions=(2.538068, 3.517241),
            position_offset=(5.1, 201.4),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 2.5)],
            overflow='NONE',
            align_y='TOP',
        ),
        RenderCase(
            name="valign_top_baseline",
            text="\n".join(generate_random_words(4, seed=70)),
            size=(200, 270),
            expected_dimensions=(2.538068, 3.517241),
            position_offset=(5.1, 224.6),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 2.5)],
            overflow='NONE',
            align_y='TOP_BASELINE',
        ),
        RenderCase(
            name="valign_center",
            text="\n".join(generate_random_words(4, seed=70)),
            size=(200, 270),
            expected_dimensions=(2.538068, 3.517241),
            position_offset=(5.1, 152.9),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 2.5)],
            overflow='NONE',
            align_y='CENTER',
        ),
        RenderCase(
            name="valign_bottom_baseline",
            text="\n".join(generate_random_words(4, seed=70)),
            size=(200, 270),
            expected_dimensions=(2.538068, 3.517241),
            position_offset=(5.1, 116.6),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 2.5)],
            overflow='NONE',
            align_y='BOTTOM_BASELINE',
        ),
        RenderCase(
            name="valign_bottom",
            text="\n".join(generate_random_words(4, seed=70)),
            size=(200, 270),
            expected_dimensions=(2.538068, 3.517241),
            position_offset=(5.1, 104.5),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 2.5)],
            overflow='NONE',
            align_y='BOTTOM',
        ),
    ]


class TestScaleToFit(TestImageComparison_MixIn, unittest.TestCase):
    """Test scale-to-fit overflow mode (text scales down to fit the box)."""

    name_prefix = "scale_to_fit."
    cases = [
        # Long text scaled to fit a narrow box.
        RenderCase(
            name="narrow",
            text=" ".join(generate_random_words(6, seed=80)),
            size=(230, 70),
            expected_dimensions=(2.974187, 0.678019),
            position_offset=(5.7, 42.1),
            text_boxes=[TextBox(0.0, 0.0, 3.0, 1.0)],
            overflow='SCALE',
        ),
        # Multiline text scaled down.
        RenderCase(
            name="multiline",
            text="\n".join(generate_random_words(4, seed=90)),
            size=(110, 110),
            expected_dimensions=(1.273771, 1.324471),
            position_offset=(8.6, 88.2),
            text_boxes=[TextBox(0.0, 0.0, 3.0, 1.5)],
            overflow='SCALE',
        ),
        # Text that already fits (no scaling needed).
        RenderCase(
            name="fits",
            text=" ".join(generate_random_words(1, seed=100)),
            size=(230, 50),
            expected_dimensions=(2.933685, 0.452255),
            position_offset=(5.2, 8.5),
            text_boxes=[TextBox(0.0, 0.0, 3.0, 2.0)],
            overflow='SCALE',
        ),
        # Combining characters with scale-to-fit.
        RenderCase(
            name="combining",
            text=unicodedata.normalize("NFD", "\u00C0vatar \u00C8ve \u0178olanda"),
            size=(140, 70),
            expected_dimensions=(1.742062, 0.768863),
            position_offset=(6.4, 43.3),
            text_boxes=[TextBox(0.0, 0.0, 2.0, 1.0)],
            overflow='SCALE',
        ),
        # Alignment variations.
        RenderCase(
            name="align_left",
            text="\n".join(generate_random_words(4, seed=70)),
            size=(60, 80),
            expected_dimensions=(0.634517, 0.879310),
            position_offset=(6.3, 62.2),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 1.0)],
            overflow='SCALE',
            align_x='LEFT',
        ),
        RenderCase(
            name="align_center",
            text="\n".join(generate_random_words(4, seed=70)),
            size=(60, 80),
            expected_dimensions=(0.629566, 0.879310),
            position_offset=(-60.4, 62.2),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 1.0)],
            overflow='SCALE',
            align_x='CENTER',
        ),
        RenderCase(
            name="align_right",
            text="\n".join(generate_random_words(4, seed=70)),
            size=(70, 80),
            expected_dimensions=(0.640662, 0.879310),
            position_offset=(-122.7, 62.2),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 1.0)],
            overflow='SCALE',
            align_x='RIGHT',
        ),
        RenderCase(
            name="align_justify",
            text="\n".join(generate_random_words(4, seed=70)),
            size=(60, 80),
            expected_dimensions=(0.634517, 0.879310),
            position_offset=(6.3, 62.2),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 1.0)],
            overflow='SCALE',
            align_x='JUSTIFY',
        ),
        RenderCase(
            name="align_flush",
            text="\n".join(generate_random_words(4, seed=70)),
            size=(200, 80),
            expected_dimensions=(2.487794, 0.879310),
            position_offset=(9.3, 62.2),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 1.0)],
            overflow='SCALE',
            align_x='FLUSH',
        ),
        # Vertical alignment variations.
        RenderCase(
            name="valign_top",
            text="\n".join(generate_random_words(4, seed=70)),
            size=(60, 80),
            expected_dimensions=(0.634517, 0.879310),
            position_offset=(6.3, 56.3),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 1.0)],
            overflow='SCALE',
            align_y='TOP',
        ),
        RenderCase(
            name="valign_top_baseline",
            text="\n".join(generate_random_words(4, seed=70)),
            size=(60, 80),
            expected_dimensions=(0.634517, 0.879310),
            position_offset=(6.3, 62.2),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 1.0)],
            overflow='SCALE',
            align_y='TOP_BASELINE',
        ),
        RenderCase(
            name="valign_center",
            text="\n".join(generate_random_words(4, seed=70)),
            size=(60, 80),
            expected_dimensions=(0.634517, 0.879310),
            position_offset=(6.3, 57.7),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 1.0)],
            overflow='SCALE',
            align_y='CENTER',
        ),
        RenderCase(
            name="valign_bottom_baseline",
            text="\n".join(generate_random_words(4, seed=70)),
            size=(60, 80),
            expected_dimensions=(0.634517, 0.879310),
            position_offset=(6.3, 62.2),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 1.0)],
            overflow='SCALE',
            align_y='BOTTOM_BASELINE',
        ),
        RenderCase(
            name="valign_bottom",
            text="\n".join(generate_random_words(4, seed=70)),
            size=(60, 80),
            expected_dimensions=(0.634517, 0.879310),
            position_offset=(6.3, 59.1),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 1.0)],
            overflow='SCALE',
            align_y='BOTTOM',
        ),
    ]


class TestTruncate(TestImageComparison_MixIn, unittest.TestCase):
    """Test truncate overflow mode (text beyond the box is not rendered)."""

    name_prefix = "truncate."
    cases = [
        # Explicit newlines, box tall enough for 3 lines, truncates the rest.
        RenderCase(
            name="cut_lines",
            text="\n".join(generate_random_words(6, seed=110)),
            size=(160, 210),
            expected_dimensions=(2.021851, 2.674291),
            position_offset=(4.7, 162.8),
            text_boxes=[TextBox(0.0, 0.0, 3.0, 3.0)],
            overflow='TRUNCATE',
        ),
        # Long single line wraps into multiple lines, box truncates after 2 lines.
        RenderCase(
            name="cut_wrap",
            text=" ".join(generate_random_words(12, seed=120)),
            size=(200, 130),
            expected_dimensions=(2.498122, 1.556504),
            position_offset=(7.1, 83.4),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 2.5)],
            overflow='TRUNCATE',
        ),
        # Combining characters wrap then truncate.
        RenderCase(
            name="cut_combining",
            text=unicodedata.normalize(
                "NFD",
                "\u00C0vatar \u00C8ve \u0178olanda r\u00E9sum\u00E9 na\u00EFve caf\u00E9",
            ),
            size=(170, 130),
            expected_dimensions=(2.101741, 1.537385),
            position_offset=(7.8, 81.6),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 2.5)],
            overflow='TRUNCATE',
        ),
        # Alignment variations (wraps then truncates).
        RenderCase(
            name="align_left",
            text=" ".join(generate_random_words(10, seed=130)),
            size=(170, 130),
            expected_dimensions=(2.118812, 1.517583),
            position_offset=(6.7, 82.6),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 2.5)],
            overflow='TRUNCATE',
            align_x='LEFT',
        ),
        RenderCase(
            name="align_center",
            text=" ".join(generate_random_words(10, seed=130)),
            size=(170, 130),
            expected_dimensions=(2.118812, 1.517583),
            position_offset=(-5.7, 82.6),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 2.5)],
            overflow='TRUNCATE',
            align_x='CENTER',
        ),
        RenderCase(
            name="align_right",
            text=" ".join(generate_random_words(10, seed=130)),
            size=(170, 130),
            expected_dimensions=(2.118812, 1.517583),
            position_offset=(-18.2, 82.6),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 2.5)],
            overflow='TRUNCATE',
            align_x='RIGHT',
        ),
        RenderCase(
            name="align_justify",
            text=" ".join(generate_random_words(10, seed=130)),
            size=(170, 130),
            expected_dimensions=(2.118812, 1.517583),
            position_offset=(6.7, 82.6),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 2.5)],
            overflow='TRUNCATE',
            align_x='JUSTIFY',
        ),
        RenderCase(
            name="align_flush",
            text=" ".join(generate_random_words(10, seed=130)),
            size=(200, 130),
            expected_dimensions=(2.464152, 1.517583),
            position_offset=(9.7, 82.6),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 2.5)],
            overflow='TRUNCATE',
            align_x='FLUSH',
        ),
        # Vertical alignment variations (wraps then truncates).
        RenderCase(
            name="valign_top",
            text=" ".join(generate_random_words(10, seed=130)),
            size=(170, 130),
            expected_dimensions=(2.118812, 1.517583),
            position_offset=(6.7, 59.4),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 2.5)],
            overflow='TRUNCATE',
            align_y='TOP',
        ),
        RenderCase(
            name="valign_top_baseline",
            text=" ".join(generate_random_words(10, seed=130)),
            size=(170, 130),
            expected_dimensions=(2.118812, 1.517583),
            position_offset=(6.7, 82.6),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 2.5)],
            overflow='TRUNCATE',
            align_y='TOP_BASELINE',
        ),
        RenderCase(
            name="valign_center",
            text=" ".join(generate_random_words(10, seed=130)),
            size=(170, 130),
            expected_dimensions=(2.118812, 1.517583),
            position_offset=(6.7, 83.0),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 2.5)],
            overflow='TRUNCATE',
            align_y='CENTER',
        ),
        RenderCase(
            name="valign_bottom_baseline",
            text=" ".join(generate_random_words(10, seed=130)),
            size=(170, 130),
            expected_dimensions=(2.118812, 1.517583),
            position_offset=(6.7, 118.6),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 2.5)],
            overflow='TRUNCATE',
            align_y='BOTTOM_BASELINE',
        ),
        RenderCase(
            name="valign_bottom",
            text=" ".join(generate_random_words(10, seed=130)),
            size=(170, 130),
            expected_dimensions=(2.118812, 1.517583),
            position_offset=(6.7, 106.5),
            text_boxes=[TextBox(0.0, 0.0, 2.5, 2.5)],
            overflow='TRUNCATE',
            align_y='BOTTOM',
        ),
    ]


# ------------------------------------------------------------------------------
# Argument Parser

def argparse_create() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--generate",
        action="store_true",
        help="Generate reference images instead of comparing",
    )
    parser.add_argument(
        "--show-html",
        type=str,
        default=None,
        metavar="PATH",
        help="Write an HTML report of failures to PATH",
    )
    return parser


# ---------------------------------------------------------------------------
# Main

def main() -> None:
    global USE_GENERATE_TEST_DATA, SHOW_HTML, OUTPUT_DIR, FONT

    if "--" in sys.argv:
        argv = [sys.argv[0]] + sys.argv[sys.argv.index("--") + 1:]
    else:
        argv = sys.argv

    parser = argparse_create()
    args, remaining = parser.parse_known_args(argv)

    USE_GENERATE_TEST_DATA = args.generate
    SHOW_HTML = args.show_html or ""

    os.makedirs(RENDER_DIR, exist_ok=True)

    FONT = load_font()

    output_ctx: contextlib.AbstractContextManager[str]
    if SHOW_HTML:
        # Write rendered/diff images into a sibling `output` subdir so the reference
        # PNGs at the top level stay clean.
        output_dir_path = os.path.join(RENDER_DIR, "output")
        os.makedirs(output_dir_path, exist_ok=True)
        output_ctx = contextlib.nullcontext(output_dir_path)
    else:
        output_ctx = tempfile.TemporaryDirectory()

    with output_ctx as output_dir:
        OUTPUT_DIR = output_dir
        unittest.main(argv=remaining, exit=False)
        if SHOW_HTML:
            if COMPARE_IMAGES:
                write_html_report(SHOW_HTML)
            else:
                sys.stderr.write("No images generated as part of running tests")

    bpy.data.fonts.remove(FONT)
    FONT = None


if __name__ == "__main__":
    main()
