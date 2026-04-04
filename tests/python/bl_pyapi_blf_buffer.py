# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Tests for the BLF (Blender Font) Python API, focusing on combining character
support: correct kerning after combining marks, and proper mark positioning.

Usage:
  ./blender.bin --background --factory-startup --python tests/python/bl_pyapi_blf.py

To regenerate reference images:
  ./blender.bin --background --factory-startup --python tests/python/bl_pyapi_blf.py -- --generate

To generate an HTML report on failure:
  ./blender.bin --background --factory-startup --python tests/python/bl_pyapi_blf.py -- --show-html /tmp/blf_report.html
"""
__all__ = (
    "main",
)

import argparse
import contextlib
import os
import random
import shutil
import subprocess
import sys
import tempfile
import unicodedata
import unittest

from typing import NamedTuple

import blf  # type: ignore[import-not-found]
import imbuf  # type: ignore[import-not-found]


# ------------------------------------------------------------------------------
# Constants


FONT_NAME: str = "Inter.woff2"
FONT_SIZE: int = 72
# Set to e.g. 4 to scale up rendering for visual inspection.
TEST_SCALE: int = 1


SOURCE_DIR: str = os.path.abspath(os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..")))
TEST_DIR: str = os.path.join(SOURCE_DIR, "tests", "files", "blenfont")
RENDER_DIR: str = os.path.join(TEST_DIR, "blf_buffer_renders")
IDIFF_BIN: str = os.environ.get("IDIFF_BIN") or shutil.which("idiff") or ""


# ------------------------------------------------------------------------------
# Globals

USE_GENERATE_TEST_DATA: bool = False
SHOW_HTML: str = ""
COMPARE_IMAGES: list["ComparedImage"] = []
OUTPUT_DIR: str = ""


# ------------------------------------------------------------------------------
# Types

# Collected data used to populate the HTML.
class ComparedImage(NamedTuple):
    #: Test class name, used to group entries in the HTML report.
    group: str
    # Name, used for the `subTest` & output filename.
    name: str
    # The text being tested.
    text: str
    # "Golden" reference image, if we differ from this - the test fails.
    ref_path: str
    # The generated output to test against.
    test_path: str
    passed: bool
    # Text output from `idiff`.
    idiff_output: str


# Data needed for a render test.
class RenderCase(NamedTuple):
    name: str
    text: str
    size: tuple[int, int]
    expected_dimensions: tuple[int, int]
    position_offset: tuple[int, int]
    font_size: int = FONT_SIZE
    wrap_width: int = -1


# ------------------------------------------------------------------------------
# Internal Utilities

def generate_lorem_combining(word_count: int, seed: int) -> str:
    """Generate deterministic lorem ipsum text with combining characters."""
    words = [
        "lo\u0300rem", "i\u0301psum", "dolo\u0301r", "si\u0300t", "a\u0300met",
        "conse\u0301ctetur", "adipi\u0300scing", "e\u0300lit", "se\u0301d",
        "ei\u0300usmod", "tempo\u0301r", "incidi\u0301dunt", "labo\u0300re",
        "dolo\u0300re", "ma\u0301gna", "ali\u0300qua",
    ]
    rng = random.Random(seed)
    return " ".join(rng.choice(words) for _ in range(word_count))


def generate_stress_text(word_size: int = 8) -> str:
    """Generate a stress-test string from various Unicode blocks, grouped into words."""
    codepoints: list[int] = []

    # Latin block: U+0001 - U+00FF (skip null terminator).
    codepoints.extend(range(0x01, 0x100))

    # Combining Diacriticals: U+0300 - U+036F.
    codepoints.extend(range(0x0300, 0x0370))

    # Number Forms / Roman Numerals: U+2150 - U+218F.
    codepoints.extend(range(0x2150, 0x2190))

    # CJK Unified Ideographs (small sample): U+4E00 - U+4E20.
    codepoints.extend(range(0x4E00, 0x4E20))

    # Halfwidth / Fullwidth Forms (sample): U+FF01 - U+FF20.
    codepoints.extend(range(0xFF01, 0xFF20))

    # Noncharacters: U+FDD0 - U+FDEF.
    codepoints.extend(range(0xFDD0, 0xFDF0))

    # Supplementary plane (4-byte UTF-8): Emoji U+1F600 - U+1F610.
    codepoints.extend(range(0x1F600, 0x1F610))

    # Mathematical Alphanumeric (sample): U+1D400 - U+1D410.
    codepoints.extend(range(0x1D400, 0x1D410))

    # Build words of `word_size` characters separated by spaces.
    words = []
    for i in range(0, len(codepoints), word_size):
        word = "".join(chr(c) for c in codepoints[i:i + word_size])
        words.append(word)
    return " ".join(words)


def generate_combining_stress(base: str, marks: str, count: int) -> str:
    """Generate a string with ``count`` combining marks stacked on ``base``."""
    return base + (marks * count)


def load_font() -> int:
    font_path = os.path.join(SOURCE_DIR, "release", "datafiles", "fonts", FONT_NAME)
    font_id: int = blf.load(font_path)
    if font_id < 0:
        raise RuntimeError("Could not load font: {:s}".format(font_path))
    blf.size(font_id, FONT_SIZE)
    blf.enable(font_id, blf.NO_FALLBACK)
    return font_id


def render_text_buffer(
        font_id: int, text: str, image_size: tuple[int, int],
        font_size: int, wrap_width: int, position_offset: tuple[int, int],
) -> object:
    """Render text into an imbuf via BLF buffer drawing."""
    blf.size(font_id, font_size)
    ibuf = imbuf.new(image_size)
    ibuf.file_type = "PNG"
    ibuf.compress = 100
    # Fill with opaque black so text is visible without a dark page background.
    ibuf.ensure_buffer("BYTE")
    with ibuf.with_buffer("BYTE", write=True) as buf:
        buf[:] = b"\x00\x00\x00\xff" * (image_size[0] * image_size[1])
    blf.color(font_id, 1.0, 1.0, 1.0, 1.0)
    if wrap_width >= 0:
        blf.enable(font_id, blf.WORD_WRAP)
        blf.word_wrap(font_id, wrap_width)
    with blf.bind_imbuf(font_id, ibuf, display_name="sRGB"):
        blf.position(font_id, position_offset[0], (image_size[1] - font_size) + position_offset[1], 0)
        blf.draw_buffer(font_id, text)
    if wrap_width >= 0:
        blf.disable(font_id, blf.WORD_WRAP)
    return ibuf


import gpu  # type: ignore[import-not-found]
import gpu.state  # type: ignore[import-not-found]
import gpu.matrix  # type: ignore[import-not-found]
try:
    gpu.init()
except SystemError as ex:
    if os.environ.get("WITHOUT_GPU"):
        gpu = None
    else:
        sys.exit("GPU initialization failed: {:s}".format(str(ex)))


def render_text_gpu(
        font_id: int, text: str, image_size: tuple[int, int],
        font_size: int, wrap_width: int, position_offset: tuple[int, int],
) -> object:
    """Render text via the GPU draw path into an imbuf."""
    import mathutils  # type: ignore[import-not-found]

    blf.size(font_id, font_size)
    blf.color(font_id, 1.0, 1.0, 1.0, 1.0)
    if wrap_width >= 0:
        blf.enable(font_id, blf.WORD_WRAP)
        blf.word_wrap(font_id, wrap_width)

    w, h = image_size
    offscreen = gpu.types.GPUOffScreen(w, h)
    with offscreen.bind():
        fb = gpu.state.active_framebuffer_get()
        fb.clear(color=(0.0, 0.0, 0.0, 1.0))
        gpu.state.blend_set("ALPHA")
        gpu.state.viewport_set(0, 0, w, h)
        gpu.matrix.load_matrix(mathutils.Matrix.Identity(4))
        gpu.matrix.load_projection_matrix(
            mathutils.Matrix(
                ((2 / w, 0, 0, -1),
                 (0, 2 / h, 0, -1),
                 (0, 0, -1, 0),
                 (0, 0, 0, 1)),
            )
        )
        blf.position(font_id, position_offset[0], (h - font_size) + position_offset[1], 0)
        blf.draw(font_id, text)
        gpu.state.blend_set("NONE")

    if wrap_width >= 0:
        blf.disable(font_id, blf.WORD_WRAP)

    pixel_buf = offscreen.texture_color.read()
    offscreen.free()

    ibuf = imbuf.new(image_size)
    ibuf.file_type = "PNG"
    ibuf.compress = 100
    ibuf.ensure_buffer("BYTE")
    gpu_bytes = bytearray(memoryview(pixel_buf).tobytes())
    expected_len = w * h * 4
    assert len(gpu_bytes) == expected_len, "GPU buffer {:d} != expected {:d}".format(len(gpu_bytes), expected_len)
    # Force alpha opaque (readback alpha may differ from the buffer path).
    for i in range(3, len(gpu_bytes), 4):
        gpu_bytes[i] = 255
    with ibuf.with_buffer("BYTE", write=True) as pixels:
        pixels[:] = gpu_bytes
    return ibuf


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
            "<title>BLF Test Report</title>\n"
            "</head><body bgcolor='#333' text='white'>\n"
            "<h1>BLF Test Report</h1>\n"
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

            # Strip redundant lines from idiff output, keep only the stats.
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
# Tests

class TestImageComparison_MixIn:
    """
    Base class for tests that compare rendered images against references.

    Sub-classes must define a ``cases`` class attribute (list of :class:`RenderCase`),
    and inherit from both this class and :class:`unittest.TestCase`.
    """

    # To be overridden.
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
        self.font_id = load_font()

    @staticmethod
    def _scale_case(case: RenderCase) -> RenderCase:
        """Apply TEST_SCALE to a case's pixel-dependent fields."""
        s = TEST_SCALE
        return case._replace(
            size=(case.size[0] * s, case.size[1] * s),
            font_size=case.font_size * s,
            wrap_width=case.wrap_width * s if case.wrap_width >= 0 else -1,
            position_offset=(case.position_offset[0] * s, case.position_offset[1] * s),
        )

    def test_render_buffer(self) -> None:
        """Buffer-rendered text should match reference images."""
        assert isinstance(self, unittest.TestCase)
        for case in self.cases:
            with self.subTest(name=case.name):
                case = self._scale_case(case)
                ibuf = render_text_buffer(
                    self.font_id, case.text, case.size,
                    case.font_size, case.wrap_width, case.position_offset,
                )
                self._compare_image(case.name, case.text, ibuf, "_buffer")

    def test_render_gpu(self) -> None:
        """GPU-rendered text should match reference images."""
        assert isinstance(self, unittest.TestCase)
        if gpu is None:
            self.skipTest("GPU not available")
        for case in self.cases:
            with self.subTest(name=case.name):
                case = self._scale_case(case)
                ibuf = render_text_gpu(
                    self.font_id, case.text, case.size,
                    case.font_size, case.wrap_width, case.position_offset,
                )
                self._compare_image(case.name, case.text, ibuf, "_gpu")

    def test_dimensions(self) -> None:
        """Dimensions should match expected values."""
        assert isinstance(self, unittest.TestCase)
        for case in self.cases:
            with self.subTest(name=case.name):
                blf.size(self.font_id, case.font_size)
                if case.wrap_width >= 0:
                    blf.enable(self.font_id, blf.WORD_WRAP)
                    blf.word_wrap(self.font_id, case.wrap_width)
                w, h = blf.dimensions(self.font_id, case.text)
                if case.wrap_width >= 0:
                    blf.disable(self.font_id, blf.WORD_WRAP)
                self.assertEqual(
                    (int(w), int(h)), case.expected_dimensions,
                    "Dimensions mismatch for {:s}: got ({:.0f}, {:.0f}) expected {!r}".format(
                        case.name, w, h, case.expected_dimensions,
                    ),
                )

    def _compare_image(self, name: str, text: str, ibuf: object, suffix: str) -> None:
        """
        Compare rendered image against a reference using idiff, or generate if --generate.

        The reference image has no suffix (one golden file per case).
        The suffix is only used for test output so buffer/GPU outputs don't collide.
        """
        assert isinstance(self, unittest.TestCase)
        name_full = self.name_prefix + name
        ref_path = os.path.join(RENDER_DIR, "{:s}.png".format(name_full))
        out_path = os.path.join(OUTPUT_DIR, "{:s}{:s}_test.png".format(name_full, suffix))

        if USE_GENERATE_TEST_DATA:
            # Reference images are generated from the buffer path only.
            # GPU must match the same reference - any difference is a bug.
            if suffix == "_buffer":
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
            name="{:s}{:s}".format(name_full, suffix),
            text=text,
            ref_path=ref_path,
            test_path=out_path,
            passed=passed,
            idiff_output=result.stdout.rstrip(),
        ))

        self.assertEqual(
            result.returncode, 0,
            "Image {:s}{:s} differs from reference:\n{:s}".format(name_full, suffix, result.stdout),
        )


class TestCombiningKerning(TestImageComparison_MixIn, unittest.TestCase):
    """Test that kerning is correct after combining characters."""

    name_prefix = "combining_kerning."
    cases = [
        RenderCase(
            name="XV",
            text="X\u0308V",
            size=(300, 100),
            expected_dimensions=(93, 54),
            position_offset=(10, 0),
        ),
        RenderCase(
            name="ro",
            text="r\u0308o",
            size=(300, 100),
            expected_dimensions=(72, 54),
            position_offset=(10, 0),
        ),
        RenderCase(
            name="Yolanda",
            text=unicodedata.normalize("NFD", "\u0178olanda"),
            size=(300, 100),
            expected_dimensions=(257, 54),
            position_offset=(10, 0),
        ),
        RenderCase(
            name="Avatar",
            text=unicodedata.normalize("NFD", "\u00C0vatar"),
            size=(300, 100),
            expected_dimensions=(203, 54),
            position_offset=(10, 0),
        ),
        RenderCase(
            name="Eve",
            text=unicodedata.normalize("NFD", "\u00C8ve"),
            size=(300, 100),
            expected_dimensions=(118, 54),
            position_offset=(10, 0),
        ),
    ]


class TestStrangeCharacters(TestImageComparison_MixIn, unittest.TestCase):
    """Test that control characters, zero-width characters, and edge cases don't crash."""

    name_prefix = "strange_characters."
    cases = [
        RenderCase(
            name="tab",
            text="A\tB",
            size=(200, 100),
            expected_dimensions=(112, 53),
            position_offset=(10, 0),
        ),
        RenderCase(
            name="carriage_return",
            text="A\rB",
            size=(200, 100),
            expected_dimensions=(93, 53),
            position_offset=(10, 0),
        ),
        RenderCase(
            name="line_feed",
            text="A\nB",
            size=(200, 100),
            expected_dimensions=(93, 53),
            position_offset=(10, 0),
        ),
        RenderCase(
            name="cr_lf",
            text="A\r\nB",
            size=(200, 100),
            expected_dimensions=(93, 53),
            position_offset=(10, 0),
        ),
        RenderCase(
            name="backspace",
            text="A\x08B",
            size=(200, 100),
            expected_dimensions=(112, 53),
            position_offset=(10, 0),
        ),
        RenderCase(
            name="escape",
            text="A\x1bB",
            size=(200, 100),
            expected_dimensions=(112, 53),
            position_offset=(10, 0),
        ),
        RenderCase(
            name="bom",
            text="A\ufeffB",
            size=(200, 100),
            expected_dimensions=(93, 53),
            position_offset=(10, 0),
        ),
        RenderCase(
            name="zero_width_space",
            text="A\u200bB",
            size=(200, 100),
            expected_dimensions=(93, 53),
            position_offset=(10, 0),
        ),
        RenderCase(
            name="zero_width_joiner",
            text="A\u200dB",
            size=(200, 100),
            expected_dimensions=(150, 78),
            position_offset=(10, 0),
        ),
        RenderCase(
            name="replacement_char",
            text="A\ufffdB",
            size=(200, 100),
            expected_dimensions=(150, 78),
            position_offset=(10, 0),
        ),
        RenderCase(
            name="empty_string",
            text="",
            size=(200, 100),
            expected_dimensions=(0, 0),
            position_offset=(10, 0),
        ),
        RenderCase(
            name="tiny_buffer",
            text="A\u0308B",
            size=(1, 1),
            expected_dimensions=(93, 54),
            position_offset=(10, 0),
        ),
        RenderCase(
            name="only_combining",
            text="\u0308\u0301",
            size=(200, 100),
            expected_dimensions=(32, 11),
            position_offset=(10, 0),
        ),
        RenderCase(
            name="double_combining",
            text="U\u0308\u0301B",
            size=(200, 100),
            expected_dimensions=(97, 54),
            position_offset=(10, 0),
        ),
    ]


class TestCombiningWordWrap(TestImageComparison_MixIn, unittest.TestCase):
    """
    Test that word-wrap works correctly with combining characters.
    """

    name_prefix = "combining_word_wrap."
    cases = [
        RenderCase(
            name="combining",
            text=unicodedata.normalize("NFD", "\u00C8ve \u00C0vatar \u0178olanda r\u00E9sum\u00E9 na\u00EFve"),
            size=(280, 500),
            expected_dimensions=(257, 402),
            wrap_width=230,
            position_offset=(10, 0),
        ),
        # Combining mark right at the wrap boundary.
        RenderCase(
            name="boundary",
            text=unicodedata.normalize("NFD", "\u0178olanda \u0178olanda \u0178olanda"),
            size=(300, 250),
            expected_dimensions=(257, 228),
            wrap_width=280,
            position_offset=(10, 0),
        ),
        # Single long word that can't wrap, with many combining marks.
        RenderCase(
            name="no_break",
            text=unicodedata.normalize("NFD", "\u00C0v\u00E0t\u00E0r\u00E0v\u00E0t\u00E0r"),
            size=(420, 150),
            expected_dimensions=(396, 54),
            wrap_width=180,
            position_offset=(10, 0),
        ),
        # Lorem ipsum paragraph with combining characters at small font size.
        RenderCase(
            name="lorem",
            text=generate_lorem_combining(280, seed=42),
            size=(420, 460),
            expected_dimensions=(390, 430),
            font_size=12,
            wrap_width=400,
            position_offset=(10, 0),
        ),
        # Stress test: Latin block + Roman Numerals, grouped into 8-char words.
        RenderCase(
            name="stress",
            text=generate_stress_text(word_size=8),
            size=(420, 220),
            expected_dimensions=(370, 191),
            font_size=12,
            wrap_width=400,
            position_offset=(10, 0),
        ),
        # Tiny font size with word wrap, combining lorem ipsum in a 100x100 image.
        RenderCase(
            name="font_size_1",
            text=generate_lorem_combining(640, seed=7),
            size=(64, 64),
            expected_dimensions=(61, 45),
            font_size=1,
            wrap_width=62,
            position_offset=(1, 0),
        ),
        # Wrap width of 1 forces a break after every space.
        RenderCase(
            name="width_1",
            text=unicodedata.normalize("NFD", "\u00C0v \u00C8x"),
            size=(100, 500),
            expected_dimensions=(84, 141),
            wrap_width=1,
            position_offset=(10, 0),
        ),
    ]


class TestMultiline(TestImageComparison_MixIn, unittest.TestCase):
    """Test multi-line text with literal newlines and combining characters."""

    name_prefix = "multiline."
    cases = [
        RenderCase(
            name="combining",
            text=unicodedata.normalize("NFD", "\u00C8ve\n\u0178olanda"),
            size=(400, 250),
            expected_dimensions=(375, 54),
            wrap_width=9999,
            position_offset=(10, 0),
        ),
        RenderCase(
            name="plain",
            text="Hello\nWorld",
            size=(400, 250),
            expected_dimensions=(353, 53),
            wrap_width=9999,
            position_offset=(10, 0),
        ),
        # Combining mark on the last character before a newline.
        RenderCase(
            name="combining_before_newline",
            text="X\u0308\nY\u0308",
            size=(200, 250),
            expected_dimensions=(92, 54),
            wrap_width=9999,
            position_offset=(10, 0),
        ),
        # Combining mark on the first character after a newline.
        RenderCase(
            name="combining_after_newline",
            text="A\nU\u0308B",
            size=(200, 250),
            expected_dimensions=(144, 54),
            wrap_width=9999,
            position_offset=(10, 0),
        ),
        # Newline-only strings with wrap enabled should have zero dimensions.
        RenderCase(
            name="newline_only",
            text="\n",
            size=(100, 100),
            expected_dimensions=(0, 0),
            wrap_width=9999,
            position_offset=(10, 0),
        ),
        RenderCase(
            name="newline_double",
            text="\n\n",
            size=(100, 100),
            expected_dimensions=(0, 0),
            wrap_width=9999,
            position_offset=(10, 0),
        ),
        RenderCase(
            name="cr_lf_only",
            text="\r\n",
            size=(100, 100),
            expected_dimensions=(0, 0),
            wrap_width=9999,
            position_offset=(10, 0),
        ),
    ]


class TestCombiningPosition(TestImageComparison_MixIn, unittest.TestCase):
    """
    Test that combining marks are visually positioned over their base character.

    Renders text into an image and compares against reference PNGs using idiff.
    Failed comparisons leave ``*_test.png`` files in the render directory.
    """

    name_prefix = "combining_position."
    cases = [
        RenderCase(
            name="diaeresis_Y",
            text="Y\u0308",
            size=(75, 100),
            expected_dimensions=(46, 54),
            position_offset=(10, 0),
        ),
        RenderCase(
            name="grave_A",
            text="A\u0300",
            size=(75, 100),
            expected_dimensions=(47, 54),
            position_offset=(10, 0),
        ),
        RenderCase(
            name="acute_e",
            text="e\u0301",
            size=(75, 100),
            expected_dimensions=(39, 54),
            position_offset=(10, 0),
        ),
        RenderCase(
            name="phrase_combining",
            text=unicodedata.normalize("NFD", "\u00C0vatar \u00C8ve \u0178olanda"),
            size=(700, 100),
            expected_dimensions=(616, 54),
            position_offset=(10, 0),
        ),
    ]


class TestStressCombining(TestImageComparison_MixIn, unittest.TestCase):
    """Stress tests for combining mark handling: stacking, enclosing, half marks."""

    name_prefix = "stress_combining."
    cases = [
        # Many combining marks on one base.
        RenderCase(
            name="all_diacriticals",
            text="X" + "".join(chr(c) for c in range(0x0300, 0x0370)),
            size=(2720, 150),
            expected_dimensions=(2688, 101),
            position_offset=(10, 0),
        ),
        # Extreme stacking depth.
        RenderCase(
            name="zalgo_100",
            text=generate_combining_stress("H", "\u0308\u0301\u0300\u0302\u0303", 20) + "ello",
            size=(300, 100),
            expected_dimensions=(164, 54),
            position_offset=(10, 0),
        ),
        # Marks that span across two base characters.
        RenderCase(
            name="half_marks",
            text="A\ufe20B\ufe21C",
            size=(300, 100),
            expected_dimensions=(259, 78),
            position_offset=(10, 0),
        ),
        # Large enclosing marks around base characters.
        RenderCase(
            name="enclosing",
            text="A\u20dd B\u20de C\u20e3",
            size=(500, 150),
            expected_dimensions=(439, 100),
            position_offset=(10, 0),
        ),
        # Zero-width joiner between combining marks.
        RenderCase(
            name="cgj",
            text="A\u0308\u034f\u0301B",
            size=(300, 100),
            expected_dimensions=(93, 54),
            position_offset=(10, 0),
        ),
    ]


class TestStressComplexScripts(TestImageComparison_MixIn, unittest.TestCase):
    """Stress tests for complex scripts with combining marks in context."""

    name_prefix = "stress_complex_scripts."
    cases = [
        # RTL script with combining vowel marks.
        RenderCase(
            name="arabic",
            text="\u0628\u064e\u0633\u0650\u0645\u064f \u0627\u0644\u0644\u0651\u0647\u0650",
            size=(720, 120),
            expected_dimensions=(703, 77),
            position_offset=(10, 0),
        ),
        # Indic script with combining vowel signs and conjunct-forming virama.
        RenderCase(
            name="devanagari",
            text="\u0928\u092e\u0938\u094d\u0924\u0947",
            size=(360, 120),
            expected_dimensions=(342, 77),
            position_offset=(10, 0),
        ),
        # Combining above/below marks with no inter-word spaces.
        RenderCase(
            name="thai",
            text="\u0e2a\u0e27\u0e31\u0e2a\u0e14\u0e35\u0e04\u0e23\u0e31\u0e1a",
            size=(590, 120),
            expected_dimensions=(570, 77),
            position_offset=(10, 0),
        ),
        # Composing syllables from separate jamo components.
        RenderCase(
            name="hangul_jamo",
            text="\u1100\u1161\u11a8 \u1102\u1161\u11bc",
            size=(450, 100),
            expected_dimensions=(361, 77),
            position_offset=(10, 0),
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
            size=(350, 100),
            expected_dimensions=(228, 77),
            position_offset=(10, 0),
        ),
        # Fullwidth Latin letters (double advance).
        RenderCase(
            name="fullwidth_latin",
            text="\uff21\uff22\uff23",
            size=(300, 100),
            expected_dimensions=(171, 77),
            position_offset=(10, 0),
        ),
        # Halfwidth Katakana (single advance).
        RenderCase(
            name="halfwidth_kana",
            text="\uff76\uff77\uff78",
            size=(200, 100),
            expected_dimensions=(171, 77),
            position_offset=(10, 0),
        ),
        # Fullwidth space between CJK.
        RenderCase(
            name="fullwidth_space",
            text="\u4e16\u3000\u754c",
            size=(300, 100),
            expected_dimensions=(171, 77),
            position_offset=(10, 0),
        ),
        # CJK with combining mark followed by Latin.
        RenderCase(
            name="mixed",
            text="\u4e16\u0308Hello",
            size=(300, 100),
            expected_dimensions=(221, 78),
            position_offset=(10, 0),
        ),
        # Figure, thin, and hair spaces between Latin.
        RenderCase(
            name="unicode_spaces",
            text="A\u2007B\u2009C\u200aD",
            size=(300, 100),
            expected_dimensions=(256, 54),
            position_offset=(10, 0),
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
            size=(280, 100),
            expected_dimensions=(259, 78),
            position_offset=(10, 0),
        ),
        # BOM at start of string (should be invisible).
        RenderCase(
            name="bom_start",
            text="\ufeffHello",
            size=(250, 100),
            expected_dimensions=(164, 53),
            position_offset=(10, 0),
        ),
        # Word joiner preventing line break.
        RenderCase(
            name="word_joiner",
            text="Hello\u2060World",
            size=(450, 100),
            expected_dimensions=(410, 78),
            position_offset=(10, 0),
        ),
        # Soft hyphen inside a word.
        RenderCase(
            name="soft_hyphen",
            text="break\u00adable",
            size=(400, 100),
            expected_dimensions=(368, 78),
            position_offset=(10, 0),
        ),
        # Multiple zero-width chars in sequence.
        RenderCase(
            name="sequence",
            text="A\u200b\u200c\u200d\u200eB",
            size=(280, 100),
            expected_dimensions=(264, 78),
            position_offset=(10, 0),
        ),
        # Zero-width char between combining marks on same base.
        RenderCase(
            name="between_marks",
            text="X\u0308\u200b\u0301Y",
            size=(200, 100),
            expected_dimensions=(92, 54),
            position_offset=(10, 0),
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
            size=(280, 100),
            expected_dimensions=(259, 78),
            position_offset=(10, 0),
        ),
        # RTL override reversing Latin text.
        RenderCase(
            name="rtl_override",
            text="\u202eHello\u202c",
            size=(350, 100),
            expected_dimensions=(278, 78),
            position_offset=(10, 0),
        ),
        # Nested LTR/RTL embeddings.
        RenderCase(
            name="nested_embed",
            text="\u202aHello \u202bWorld\u202c\u202c",
            size=(650, 100),
            expected_dimensions=(600, 78),
            position_offset=(10, 0),
        ),
        # Arabic and Latin mixed with explicit bidi marks.
        RenderCase(
            name="mixed_script",
            text="Hello \u200f\u0645\u0631\u062d\u0628\u0627\u200e World",
            size=(810, 120),
            expected_dimensions=(790, 78),
            position_offset=(10, 0),
        ),
        # Unicode line and paragraph separators.
        RenderCase(
            name="line_para_sep",
            text="A\u2028B\u2029C",
            size=(300, 100),
            expected_dimensions=(259, 78),
            position_offset=(10, 0),
        ),
        # Every bidi control character in sequence.
        RenderCase(
            name="all_controls",
            text="X\u200e\u200f\u202a\u202b\u202c\u202d\u202eY",
            size=(510, 100),
            expected_dimensions=(491, 78),
            position_offset=(10, 0),
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
            size=(200, 100),
            expected_dimensions=(150, 78),
            position_offset=(10, 0),
        ),
        RenderCase(
            name="nonchar_fdef",
            text="A\ufdefB",
            size=(200, 100),
            expected_dimensions=(150, 78),
            position_offset=(10, 0),
        ),
        RenderCase(
            name="nonchar_fffe",
            text="A\ufffeB",
            size=(200, 100),
            expected_dimensions=(150, 78),
            position_offset=(10, 0),
        ),
        RenderCase(
            name="nonchar_ffff",
            text="A\uffffB",
            size=(200, 100),
            expected_dimensions=(150, 78),
            position_offset=(10, 0),
        ),
        # Specials block including unassigned codepoints.
        RenderCase(
            name="specials",
            text="\ufff0\ufff1\ufffd",
            size=(250, 120),
            expected_dimensions=(171, 77),
            position_offset=(10, 0),
        ),
        # Replacement character repeated.
        RenderCase(
            name="replacement",
            text="\ufffd\ufffd\ufffd",
            size=(250, 120),
            expected_dimensions=(171, 77),
            position_offset=(10, 0),
        ),
        # Maximum valid codepoint U+10FFFF.
        RenderCase(
            name="max_codepoint",
            text="A\U0010ffffB",
            size=(200, 100),
            expected_dimensions=(150, 78),
            position_offset=(10, 0),
        ),
    ]

    def test_surrogates_raise(self) -> None:
        """Lone surrogates should raise UnicodeEncodeError, not crash."""
        for text in ["\ud800", "\udfff", "\ud800\udc00"]:
            with self.subTest(text=repr(text)):
                with self.assertRaises(UnicodeEncodeError):
                    blf.dimensions(self.font_id, text)

    def test_embedded_null_raises(self) -> None:
        """Embedded null should raise ValueError, not silently truncate."""
        with self.assertRaises(ValueError):
            blf.dimensions(self.font_id, "A\x00B")


class TestStressSupplementary(TestImageComparison_MixIn, unittest.TestCase):
    """Stress tests for 4-byte UTF-8 (codepoints above U+FFFF)."""

    name_prefix = "stress_supplementary."
    cases = [
        # Emoji (supplementary plane).
        RenderCase(
            name="emoji",
            text="A\U0001f600B\U0001f64fC",
            size=(300, 100),
            expected_dimensions=(259, 78),
            position_offset=(10, 0),
        ),
        # Mathematical Alphanumeric Symbols.
        RenderCase(
            name="math_alpha",
            text="\U0001d400\U0001d401\U0001d402",
            size=(200, 100),
            expected_dimensions=(171, 77),
            position_offset=(10, 0),
        ),
        # Emoji Zero Width Joiner (ZWJ) sequence.
        RenderCase(
            name="emoji_zwj",
            text="\U0001f468\u200d\U0001f469\u200d\U0001f467",
            size=(350, 100),
            expected_dimensions=(285, 77),
            position_offset=(10, 0),
        ),
        # Mixed BMP and supplementary.
        RenderCase(
            name="mixed",
            text="Hello \U0001f600 World \U0001d400",
            size=(600, 100),
            expected_dimensions=(524, 78),
            position_offset=(10, 0),
        ),
    ]


class TestCombiningEdgePositions(TestImageComparison_MixIn, unittest.TestCase):
    """Test combining marks at edge positions: start, end, after space/zero-width."""

    name_prefix = "combining_edge_positions."
    cases = [
        # Combining mark as first character (no base to center on).
        RenderCase(
            name="combining_first",
            text="\u0308Hello",
            size=(250, 100),
            expected_dimensions=(184, 54),
            position_offset=(10, 0),
        ),
        # Combining mark after a space (invisible base).
        RenderCase(
            name="combining_after_space",
            text="A \u0308B",
            size=(200, 100),
            expected_dimensions=(112, 54),
            position_offset=(10, 0),
        ),
        # Combining mark at end of string (no following character).
        RenderCase(
            name="combining_at_end",
            text="Hello\u0308",
            size=(250, 100),
            expected_dimensions=(164, 54),
            position_offset=(10, 0),
        ),
        # Combining mark after zero-width space (zero-advance base).
        RenderCase(
            name="combining_after_zwsp",
            text="\u200b\u0308",
            size=(100, 100),
            expected_dimensions=(32, 53),
            position_offset=(10, 0),
        ),
        # Many combining marks with no base character at all.
        RenderCase(
            name="many_combining_no_base",
            text="\u0308\u0301\u0300\u0302\u0303\u0304\u0305\u0306\u0307\u0309",
            size=(150, 100),
            expected_dimensions=(77, 77),
            position_offset=(10, 0),
        ),
    ]


class TestCombiningSequences(TestImageComparison_MixIn, unittest.TestCase):
    """Test combining mark sequences: bursts, alternating, repeated."""

    name_prefix = "combining_sequences."
    cases = [
        # Run of base chars then burst of combining on the last.
        RenderCase(
            name="base_then_burst",
            text="ABCDE\u0308\u0301\u0300",
            size=(300, 100),
            expected_dimensions=(237, 54),
            position_offset=(10, 0),
        ),
        # Every base char has a combining mark.
        RenderCase(
            name="alternating",
            text="a\u0308b\u0308c\u0308d\u0308",
            size=(200, 100),
            expected_dimensions=(159, 54),
            position_offset=(10, 0),
        ),
        # Same combining mark repeated on one base.
        RenderCase(
            name="repeated_mark",
            text="A\u0308\u0308\u0308",
            size=(100, 100),
            expected_dimensions=(47, 54),
            position_offset=(10, 0),
        ),
    ]


class TestWrapCombiningEdge(TestImageComparison_MixIn, unittest.TestCase):
    """Test word wrap when combining marks fall near the wrap boundary."""

    name_prefix = "wrap_combining_edge."
    cases = [
        # Combining mark on the last char before wrap boundary.
        RenderCase(
            name="at_combining",
            text="WWWWW\u0308 Next",
            size=(400, 200),
            expected_dimensions=(340, 141),
            wrap_width=300,
            position_offset=(10, 0),
        ),
        # Tighter wrap forcing break right after base+combining.
        RenderCase(
            name="combining_tight",
            text="WWWW\u0308 Next",
            size=(300, 200),
            expected_dimensions=(272, 141),
            wrap_width=200,
            position_offset=(10, 0),
        ),
    ]


class TestStressEdgeParams(TestImageComparison_MixIn, unittest.TestCase):
    """Stress tests for edge-case parameter values."""

    name_prefix = "stress_edge_params."
    cases = [
        # Font size 1.
        RenderCase(
            name="font_size_1",
            text="Hello",
            size=(100, 20),
            expected_dimensions=(3, 1),
            font_size=1,
            position_offset=(10, 0),
        ),
        # Font size 2.
        RenderCase(
            name="font_size_2",
            text="Hello",
            size=(100, 20),
            expected_dimensions=(6, 1),
            font_size=2,
            position_offset=(10, 0),
        ),
        # wrap_width=0 (wraps every character).
        RenderCase(
            name="wrap_0",
            text="Hello World",
            size=(210, 230),
            expected_dimensions=(189, 140),
            wrap_width=0,
            position_offset=(10, 0),
        ),
        # Negative position (text off-screen, should not crash).
        RenderCase(
            name="negative_pos",
            text="Hello",
            size=(100, 100),
            expected_dimensions=(164, 53),
            position_offset=(-50, -50),
        ),
    ]


class TestNonUTF8(unittest.TestCase):
    """Test BLF behavior with invalid UTF-8 byte sequences passed as ``bytes``."""

    def setUp(self) -> None:
        self.font_id = load_font()

    # Truncated multi-byte sequences.

    def test_truncated_2byte(self) -> None:
        """Truncated 2-byte sequence should not crash."""
        blf.dimensions(self.font_id, b"A\xc3B")

    def test_truncated_3byte(self) -> None:
        """Truncated 3-byte sequence should not crash."""
        blf.dimensions(self.font_id, b"A\xe0\xa0B")

    def test_truncated_4byte(self) -> None:
        """Truncated 4-byte sequence should not crash."""
        blf.dimensions(self.font_id, b"A\xf0\x90\x80B")

    def test_truncated_at_end(self) -> None:
        """Truncated multi-byte at end of buffer (no following byte)."""
        for lead in [b"\xc3", b"\xe0\xa0", b"\xf0\x90\x80"]:
            with self.subTest(lead=lead):
                blf.dimensions(self.font_id, b"Hello" + lead)

    # Overlong encodings.

    def test_overlong_2byte(self) -> None:
        """Overlong 2-byte encoding of U+0000 should not crash."""
        blf.dimensions(self.font_id, b"\xc0\x80")

    def test_overlong_3byte(self) -> None:
        """Overlong 3-byte encoding of U+002F should not crash."""
        blf.dimensions(self.font_id, b"\xe0\x80\xaf")

    def test_overlong_4byte(self) -> None:
        """Overlong 4-byte encoding of U+0041 ('A')."""
        blf.dimensions(self.font_id, b"\xf0\x80\x81\x81")

    def test_overlong_all_c0_c1(self) -> None:
        """Every C0/C1 lead byte forms an overlong encoding."""
        for b in range(0xC0, 0xC2):
            with self.subTest(byte=hex(b)):
                blf.dimensions(self.font_id, bytes([b, 0x80]))

    # Continuation bytes without a lead.

    def test_continuation_without_lead(self) -> None:
        """Bare continuation byte should not crash."""
        blf.dimensions(self.font_id, b"\x80\xbf\x80")

    def test_all_continuation_bytes(self) -> None:
        """String of only continuation bytes (0x80-0xBF) should not crash."""
        blf.dimensions(self.font_id, bytes(range(0x80, 0xC0)))

    def test_continuation_after_ascii(self) -> None:
        """Continuation byte immediately after ASCII."""
        blf.dimensions(self.font_id, b"A\x80B\xbfC")

    # Invalid lead bytes.

    def test_fe_ff(self) -> None:
        """0xFE and 0xFF are never valid in UTF-8."""
        blf.dimensions(self.font_id, b"\xfe\xff")

    def test_5byte_sequence(self) -> None:
        """5-byte sequence (F8 lead) is invalid in RFC 3629 UTF-8."""
        blf.dimensions(self.font_id, b"\xf8\x80\x80\x80\x80")

    def test_6byte_sequence(self) -> None:
        """6-byte sequence (FC lead) is invalid in RFC 3629 UTF-8."""
        blf.dimensions(self.font_id, b"\xfc\x80\x80\x80\x80\x80")

    def test_all_invalid_lead_bytes(self) -> None:
        """Every byte F5-FF as a lead byte followed by continuation bytes."""
        for b in range(0xF5, 0x100):
            with self.subTest(byte=hex(b)):
                blf.dimensions(self.font_id, bytes([b, 0x80, 0x80, 0x80]))

    # Surrogates.

    def test_surrogate_bytes(self) -> None:
        """UTF-8 encoded surrogates (ED A0 80 = U+D800) should not crash."""
        blf.dimensions(self.font_id, b"\xed\xa0\x80")

    def test_surrogate_pair_bytes(self) -> None:
        """CESU-8 style surrogate pair (U+D800 U+DC00) should not crash."""
        blf.dimensions(self.font_id, b"\xed\xa0\x80\xed\xb0\x80")

    def test_all_surrogate_boundaries(self) -> None:
        """First and last surrogate: U+D800, U+DFFF."""
        blf.dimensions(self.font_id, b"\xed\xa0\x80")  # U+D800
        blf.dimensions(self.font_id, b"\xed\xbf\xbf")  # U+DFFF

    # Above U+10FFFF.

    def test_above_max_codepoint(self) -> None:
        """4-byte sequence decoding above U+10FFFF."""
        blf.dimensions(self.font_id, b"\xf4\x90\x80\x80")  # U+110000

    # Mixed valid/invalid.

    def test_mixed_valid_invalid(self) -> None:
        """Valid text surrounding invalid bytes should not crash."""
        blf.dimensions(self.font_id, b"Hello \xff\xfe World")

    def test_alternating_valid_invalid(self) -> None:
        """Alternating valid ASCII and invalid bytes."""
        data = b""
        for i in range(256):
            data += bytes([0x41, 0x80 | (i & 0x3F)])
        blf.dimensions(self.font_id, data)

    def test_valid_then_broken_mid_codepoint(self) -> None:
        """Valid 2-byte char then a lead byte with wrong continuation."""
        blf.dimensions(self.font_id, b"\xc3\xa9\xe0\x41\xc3\xa9")

    # Embedded nulls.

    def test_embedded_null_bytes(self) -> None:
        """Embedded null in bytes should not crash."""
        # NOTE: BLF currently truncates at null internally despite receiving the length.
        blf.dimensions(self.font_id, b"A\x00B")

    def test_many_nulls(self) -> None:
        """String of many null bytes."""
        blf.dimensions(self.font_id, b"\x00" * 1024)

    # Stress / bulk.

    def test_every_single_byte(self) -> None:
        """Every byte value 0x00-0xFF as a single-byte string."""
        for b in range(256):
            with self.subTest(byte=hex(b)):
                blf.dimensions(self.font_id, bytes([b]))

    def test_every_byte_pair(self) -> None:
        """All 256 byte values packed into one string."""
        blf.dimensions(self.font_id, bytes(range(256)))

    def test_random_bytes_large(self) -> None:
        """4 KB of pseudo-random bytes (deterministic seed)."""
        rng = random.Random(42)
        data = bytes(rng.randint(0, 255) for _ in range(4096))
        blf.dimensions(self.font_id, data)

    def test_random_bytes_with_wrap(self) -> None:
        """Random bytes with word-wrap enabled."""
        rng = random.Random(99)
        data = bytes(rng.randint(0, 255) for _ in range(2048))
        blf.enable(self.font_id, blf.WORD_WRAP)
        blf.word_wrap(self.font_id, 200)
        blf.dimensions(self.font_id, data)
        blf.disable(self.font_id, blf.WORD_WRAP)

    # Draw paths (buffer + GPU).

    def test_draw_buffer_invalid(self) -> None:
        """Drawing invalid UTF-8 via buffer path should not crash."""
        ibuf = imbuf.new((100, 100))
        ibuf.file_type = "PNG"
        ibuf.ensure_buffer("BYTE")
        with blf.bind_imbuf(self.font_id, ibuf, display_name="sRGB"):
            blf.draw_buffer(self.font_id, b"A\xff\xfeB")

    def test_draw_buffer_random(self) -> None:
        """Drawing random bytes via buffer path."""
        rng = random.Random(77)
        data = bytes(rng.randint(0, 255) for _ in range(512))
        ibuf = imbuf.new((200, 200))
        ibuf.file_type = "PNG"
        ibuf.ensure_buffer("BYTE")
        with blf.bind_imbuf(self.font_id, ibuf, display_name="sRGB"):
            blf.draw_buffer(self.font_id, data)

    def test_draw_gpu_invalid(self) -> None:
        """Drawing invalid UTF-8 via GPU path should not crash."""
        if gpu is None:
            self.skipTest("GPU not available")
        blf.position(self.font_id, 10, 10, 0)
        blf.draw(self.font_id, b"\xff\xfe\xf0\x80\xc0\x80Hello")

    def test_draw_gpu_random(self) -> None:
        """Drawing random bytes via GPU path."""
        if gpu is None:
            self.skipTest("GPU not available")
        rng = random.Random(55)
        data = bytes(rng.randint(0, 255) for _ in range(512))
        blf.position(self.font_id, 10, 10, 0)
        blf.draw(self.font_id, data)

    # Word-wrap with invalid bytes.

    def test_wrap_truncated_at_boundary(self) -> None:
        """Word-wrap where a truncated sequence sits at the wrap point."""
        blf.enable(self.font_id, blf.WORD_WRAP)
        blf.word_wrap(self.font_id, 50)
        blf.dimensions(self.font_id, b"AAAA \xc3 BBBB \xe0\xa0 CCCC")
        blf.disable(self.font_id, blf.WORD_WRAP)

    def test_wrap_only_invalid(self) -> None:
        """Word-wrap on a string with no valid break points, all invalid."""
        blf.enable(self.font_id, blf.WORD_WRAP)
        blf.word_wrap(self.font_id, 10)
        blf.dimensions(self.font_id, bytes(range(0x80, 0x100)))
        blf.disable(self.font_id, blf.WORD_WRAP)

    # Font state combinations with invalid bytes.

    def test_clipping_with_invalid(self) -> None:
        """Invalid bytes with clipping enabled."""
        blf.enable(self.font_id, blf.CLIPPING)
        blf.clipping(self.font_id, 0, 0, 50, 50)
        blf.dimensions(self.font_id, b"\xff\xfe\xc0\x80\xf8\x80\x80\x80\x80")
        blf.disable(self.font_id, blf.CLIPPING)

    def test_shadow_with_invalid(self) -> None:
        """Invalid bytes with shadow enabled."""
        blf.enable(self.font_id, blf.SHADOW)
        blf.shadow(self.font_id, 3, 0.0, 0.0, 0.0, 0.5)
        blf.shadow_offset(self.font_id, 2, -2)
        ibuf = imbuf.new((100, 100))
        ibuf.file_type = "PNG"
        ibuf.ensure_buffer("BYTE")
        with blf.bind_imbuf(self.font_id, ibuf, display_name="sRGB"):
            blf.draw_buffer(self.font_id, b"\xff\xfe\xc0\x80")
        blf.disable(self.font_id, blf.SHADOW)

    def test_rotation_with_invalid(self) -> None:
        """Invalid bytes with rotation enabled (GPU path)."""
        if gpu is None:
            self.skipTest("GPU not available")
        blf.enable(self.font_id, blf.ROTATION)
        blf.rotation(self.font_id, 0.785)
        blf.position(self.font_id, 10, 10, 0)
        blf.draw(self.font_id, b"\xf0\x80\x80\x80\xed\xa0\x80")
        blf.disable(self.font_id, blf.ROTATION)

    def test_monochrome_with_invalid(self) -> None:
        """Invalid bytes with monochrome rendering."""
        blf.enable(self.font_id, blf.MONOCHROME)
        blf.dimensions(self.font_id, b"\xff" * 64)
        blf.disable(self.font_id, blf.MONOCHROME)

    # Font size extremes with invalid bytes.

    def test_size_1_invalid(self) -> None:
        """Invalid bytes at font size 1."""
        blf.size(self.font_id, 1)
        blf.dimensions(self.font_id, bytes(range(256)))
        blf.size(self.font_id, FONT_SIZE)

    def test_size_large_invalid(self) -> None:
        """Invalid bytes at a very large font size."""
        blf.size(self.font_id, 500)
        blf.dimensions(self.font_id, b"\xff\xfe\xc0\x80\xf4\x90\x80\x80")
        blf.size(self.font_id, FONT_SIZE)

    # Tiny and extreme buffer sizes.

    def test_draw_buffer_1x1_invalid(self) -> None:
        """Draw invalid bytes into a 1x1 buffer."""
        ibuf = imbuf.new((1, 1))
        ibuf.file_type = "PNG"
        ibuf.ensure_buffer("BYTE")
        with blf.bind_imbuf(self.font_id, ibuf, display_name="sRGB"):
            blf.draw_buffer(self.font_id, b"\xff\xfe\xc3\xe0\xa0\xf0\x90\x80")

    def test_draw_buffer_large_random(self) -> None:
        """Draw 16 KB of random bytes into a buffer."""
        rng = random.Random(33)
        data = bytes(rng.randint(0, 255) for _ in range(16384))
        ibuf = imbuf.new((400, 400))
        ibuf.file_type = "PNG"
        ibuf.ensure_buffer("BYTE")
        with blf.bind_imbuf(self.font_id, ibuf, display_name="sRGB"):
            blf.draw_buffer(self.font_id, data)

    # Rapid repeated draws.

    def test_repeated_draws_invalid(self) -> None:
        """Many rapid draw calls with different invalid sequences."""
        ibuf = imbuf.new((100, 100))
        ibuf.file_type = "PNG"
        ibuf.ensure_buffer("BYTE")
        with blf.bind_imbuf(self.font_id, ibuf, display_name="sRGB"):
            for i in range(200):
                blf.draw_buffer(self.font_id, bytes([0x80 + (i % 128), 0xC0 + (i % 32), 0xFF]))

    def test_interleave_str_and_bytes(self) -> None:
        """Alternating str and bytes calls to check state doesn't leak."""
        for _ in range(50):
            blf.dimensions(self.font_id, "Hello")
            blf.dimensions(self.font_id, b"\xff\xfe")
            blf.dimensions(self.font_id, "World")
            blf.dimensions(self.font_id, b"\xc0\x80\xe0\x80\xaf")

    # Sliding window: invalid byte at every offset in a valid string.

    def test_sliding_invalid_byte(self) -> None:
        """Insert a single 0xFF at every position in a valid ASCII string."""
        base = b"The quick brown fox jumps over the lazy dog"
        for i in range(len(base) + 1):
            with self.subTest(offset=i):
                blf.dimensions(self.font_id, base[:i] + b"\xff" + base[i:])

    def test_sliding_truncated_4byte(self) -> None:
        """Slide a truncated 4-byte sequence through a valid string."""
        base = b"ABCDEFGHIJKLMNOP"
        for i in range(len(base) + 1):
            with self.subTest(offset=i):
                blf.dimensions(self.font_id, base[:i] + b"\xf0\x90\x80" + base[i:])

    # Invalid combining: valid combining marks after invalid lead bytes.

    def test_combining_after_invalid(self) -> None:
        """Valid combining marks (U+0308 = CC 88) after invalid lead bytes."""
        blf.dimensions(self.font_id, b"\xff\xcc\x88\xfe\xcc\x88\xc0\xcc\x88")

    def test_invalid_between_combining(self) -> None:
        """Invalid byte sandwiched between valid base+combining sequences."""
        # A + U+0308 + 0xFF + B + U+0301
        blf.dimensions(self.font_id, b"A\xcc\x88\xffB\xcc\x81")

    # Power-of-2 and boundary lengths.

    def test_boundary_lengths(self) -> None:
        """Invalid bytes at power-of-2 lengths and nearby offsets."""
        for exp in range(0, 14):
            size = 1 << exp
            for offset in (-1, 0, 1):
                length = max(1, size + offset)
                with self.subTest(length=length):
                    blf.dimensions(self.font_id, b"\xff" * length)

    # Empty bytes.

    def test_empty_bytes(self) -> None:
        """Empty bytes object."""
        w, h = blf.dimensions(self.font_id, b"")
        self.assertEqual(w, 0.0)
        self.assertEqual(h, 0.0)

    # Pathological patterns.

    def test_lead_without_continuation_every_lead(self) -> None:
        """Every possible lead byte (C2-F4) followed by a non-continuation byte."""
        for lead in range(0xC2, 0xF5):
            with self.subTest(lead=hex(lead)):
                blf.dimensions(self.font_id, bytes([lead, 0x41]))

    def test_max_continuation_run(self) -> None:
        """Lead byte followed by way too many continuation bytes."""
        # 4-byte lead followed by 100 continuation bytes.
        blf.dimensions(self.font_id, b"\xf0" + b"\x80" * 100)

    def test_all_leads_no_continuations(self) -> None:
        """Every lead byte back-to-back with no continuation bytes at all."""
        blf.dimensions(self.font_id, bytes(range(0xC0, 0xF5)))


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
    global USE_GENERATE_TEST_DATA, SHOW_HTML, OUTPUT_DIR

    if "--" in sys.argv:
        argv = [sys.argv[0]] + sys.argv[sys.argv.index("--") + 1:]
    else:
        argv = sys.argv

    parser = argparse_create()
    args, remaining = parser.parse_known_args(argv)

    USE_GENERATE_TEST_DATA = args.generate
    SHOW_HTML = args.show_html or ""

    output_ctx: contextlib.AbstractContextManager[str]
    if SHOW_HTML:
        output_ctx = contextlib.nullcontext(RENDER_DIR)
    else:
        output_ctx = tempfile.TemporaryDirectory()

    with output_ctx as output_dir:
        OUTPUT_DIR = output_dir
        unittest.main(argv=remaining, exit=False)
        if SHOW_HTML:
            if COMPARE_IMAGES:
                write_html_report(SHOW_HTML)
            else:
                # Could occur if tests are restricted to tests that don't generate images.
                sys.stderr.write("No images generated as part of running tests")


if __name__ == "__main__":
    main()
