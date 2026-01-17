#!/usr/bin/env python3
"""
Validation script for Blender font rendering against BLF texture.

Renders font characters using both curve-based EEVEE rendering and BLF texture
rendering, saving the results as PNG images for comparison.

Can be run directly or through Blender:
    ./validate_against_texture.sh                       # Test all fonts in ./fonts
    ./validate_against_texture.sh path/to/font.ttf      # Test specific font
    ./validate_against_texture.sh --font-dir DIR        # Test all fonts in DIR
    ./validate_against_texture.sh --chars 'ABC'         # Test only specific characters
"""
__all__ = (
    "main",
)

from typing import Any, NamedTuple, TYPE_CHECKING

import argparse
import os
import sys

# Detect if running inside Blender or directly
try:
    import bpy  # type: ignore[import-not-found]
    import mathutils  # type: ignore[import-not-found]
    INSIDE_BLENDER = True
except ImportError:
    bpy = None  # type: ignore[assignment]
    mathutils = None  # type: ignore[assignment]
    INSIDE_BLENDER = False

# Optional fontforge import - only used to check the characters which are available.
try:
    import fontforge  # type: ignore[import-not-found,unused-ignore]
    HAS_FONTFORGE = True
except ImportError:
    fontforge = None  # type: ignore[assignment]
    HAS_FONTFORGE = False


if TYPE_CHECKING:
    # Type aliases for Blender types (actual types not available outside Blender)
    BlenderFont = Any
    BlenderObject = Any

# Supported font file extensions
# FONT_EXTENSIONS: tuple[str, ...] = ('.ttf', '.otf', '.woff', '.woff2')
FONT_EXTENSIONS: tuple[str, ...] = ('.ttf', )

# Global log for report messages (used for error.blend text block)
_report_log: list[str] = []

# Raster output image size in pixels
RASTER_OUTPUT_SIZE: int = 1024

# BLF rendering margin in pixels (quarter image size to accommodate accents)
BLF_MARGIN_PX: int = RASTER_OUTPUT_SIZE // 4


# Output formatting
SEPARATOR_SHORT: str = "=" * 50
SEPARATOR_LONG: str = "=" * 60

DEFAULT_TEST_CHARS: str = (
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789"
    "!@#$%^&*()_+-=[]{}|;':\",./<>?`~"
)


class CharTestResult(NamedTuple):
    """Result of testing a single character."""
    char: str
    error: str | None


class FontTestResult(NamedTuple):
    """Result of testing a font."""
    passed: int
    failed: int
    failed_chars: list[CharTestResult]
    html_data: "FontHtmlData | None"  # Data for HTML generation


class JobResult(NamedTuple):
    """Result of a parallel job."""
    font: str
    returncode: int
    stdout: str
    stderr: str


class CharImageData(NamedTuple):
    """Data for a single character's rendered images."""
    char: str
    curves_filename: str
    blf_filename: str
    diff_filename: str
    passed: bool


class FontHtmlData(NamedTuple):
    """Data for generating HTML for a font."""
    font_name: str
    font_relpath: str
    output_dir: str  # Directory where images are stored
    images: list[CharImageData]


class ValidationError(Exception):
    """Raised when validation setup fails."""


def report(message: str = "", end: str = "\n") -> None:
    """Print message to stdout and append to global log."""
    print(message, end=end, flush=True)
    _report_log.append(message)


# -----------------------------------------------------------------------------
# Direct execution mode (outside Blender)

def find_fonts_dir() -> str | None:
    """Find the default fonts directory relative to this script."""
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(script_dir))))
    fonts_dir = os.path.join(project_root, "fonts")
    if os.path.isdir(fonts_dir):
        return fonts_dir
    return None


def find_font_files(directory: str) -> list[str]:
    """Find all font files in a directory recursively, ignoring dot-directories."""
    fonts = []
    for root, dirs, files in os.walk(directory):
        # Filter out directories starting with "." (modifies dirs in-place to skip them)
        dirs[:] = [d for d in dirs if not d.startswith('.')]
        for filename in files:
            if filename.lower().endswith(FONT_EXTENSIONS):
                fonts.append(os.path.join(root, filename))
    return sorted(fonts)


def run_single_font_job(
    blender: str,
    validate_py: str,
    font: str,
    extra_args: list[str],
) -> JobResult:
    """Run validation for a single font file."""
    import subprocess

    env = os.environ.copy()
    env["PYTHONUNBUFFERED"] = "1"

    cmd = [
        blender,
        "-q",
        "--background",
        "--factory-startup",
        "--log-level", "error",
        "--python",
        validate_py,
        "--",
        font,
        *extra_args,
    ]

    result = subprocess.run(cmd, env=env, capture_output=True, text=True)
    return JobResult(
        font=font,
        returncode=result.returncode,
        stdout=result.stdout,
        stderr=result.stderr,
    )


def run_parallel_jobs(
    blender: str,
    validate_py: str,
    fonts: list[str],
    extra_args: list[str],
    jobs: int,
) -> int:
    """Run validation for multiple fonts in parallel."""
    from concurrent.futures import ThreadPoolExecutor, as_completed

    max_returncode = 0
    total_fonts = len(fonts)
    num_width = len(str(total_fonts))
    completed = 0

    with ThreadPoolExecutor(max_workers=jobs) as executor:
        futures = {
            executor.submit(run_single_font_job, blender, validate_py, font, extra_args): font
            for font in fonts
        }

        for future in as_completed(futures):
            result = future.result()
            completed += 1
            if result.stdout:
                print(result.stdout, end="", flush=True)
            if result.stderr:
                print(result.stderr, end="", file=sys.stderr, flush=True)
            print("[{:0{width}d} of {:d}] {:s}".format(
                completed, total_fonts, os.path.basename(result.font), width=num_width
            ), flush=True)
            if result.returncode != 0:
                max_returncode = max(max_returncode, result.returncode)

    return max_returncode


def print_direct_help() -> None:
    """Print help message for direct execution mode."""
    print("""\
Validation script for Blender font rendering against BLF texture.

Renders font characters using both curve-based EEVEE rendering and BLF texture
rendering, saving the results as PNG images for comparison.

Usage:
  ./validate_against_texture.sh [options] [font_file]

Environment variables (direct mode only):
  BLENDER_BIN   Path to Blender executable (default: "blender")
  BLENDER_JOBS  Number of parallel jobs (default: 1, 0 = number of CPU cores)
              When > 1, runs one Blender process per font file.

Options (passed to Blender):
  font_file               Font file to test (.ttf, .otf, .woff, .woff2)
  --font-dir DIR          Test all font files in directory
  --chars CHARS           Test only specific characters (e.g., 'ABC')
  --method METHOD         Overlap removal method: FONTFORGE or SKIA (default: FONTFORGE)
  -v, --verbose           Show all results (default: only show failures)
  --blend-from-errors FILE
                          Save a .blend file showing failed characters
  -h, --help              Show this help message

Examples:
  ./validate_against_texture.sh                    Test all fonts in ./fonts
  ./validate_against_texture.sh --chars 'ABC'      Test only specific characters
  ./validate_against_texture.sh path/to/font.ttf   Test a specific font
""")


def main_direct() -> int:
    """Main entry point when running directly (outside Blender)."""
    import subprocess

    # Handle --help locally
    if "--help" in sys.argv or "-h" in sys.argv:
        print_direct_help()
        return 0

    # Get Blender path from environment or default to "blender"
    blender = os.environ.get("BLENDER_BIN") or "blender"

    # Get number of jobs from environment (0 = number of logical cores)
    jobs_str = os.environ.get("BLENDER_JOBS", "1")
    try:
        jobs = int(jobs_str)
        if jobs < 0:
            raise ValueError()
        if jobs == 0:
            jobs = os.cpu_count() or 1
    except ValueError:
        print(f"Error: BLENDER_JOBS must be a non-negative integer, got: {jobs_str}", file=sys.stderr)
        return 1

    validate_py = os.path.abspath(__file__)
    forward_args = sys.argv[1:]

    env = os.environ.copy()
    env["PYTHONUNBUFFERED"] = "1"

    # Check if --font-dir is in arguments
    font_dir: str | None = None
    for i, arg in enumerate(forward_args):
        if arg == "--font-dir" and i + 1 < len(forward_args):
            font_dir = forward_args[i + 1]
            break
        elif arg.startswith("--font-dir="):
            font_dir = arg.split("=", 1)[1]
            break

    # Check if a font file is specified (positional arg that looks like a font)
    has_font_file = any(
        arg.lower().endswith(FONT_EXTENSIONS) and not arg.startswith("-")
        for arg in forward_args
    )

    # If no font source specified, use default fonts directory
    if not font_dir and not has_font_file:
        default_fonts_dir = find_fonts_dir()
        if default_fonts_dir:
            font_dir = default_fonts_dir
            forward_args = ["--font-dir", default_fonts_dir] + forward_args
        else:
            print("Error: No font file or directory specified.", file=sys.stderr)
            return 1

    # Parallel execution: distribute fonts across jobs
    if jobs > 1 and font_dir and os.path.isdir(font_dir):
        fonts = find_font_files(font_dir)
        if len(fonts) > 1:
            # Keep --font-dir in args so it's used as cache base directory
            return run_parallel_jobs(blender, validate_py, fonts, forward_args, jobs)

    # Single job: forward all arguments to Blender
    cmd = [
        blender,
        "-q",
        "--background",
        "--factory-startup",
        "--log-level", "error",
        "--python",
        validate_py,
        "--",
        *forward_args,
    ]
    result = subprocess.run(cmd, env=env)
    return result.returncode


# -----------------------------------------------------------------------------
# Blender execution mode (inside Blender)
# -----------------------------------------------------------------------------

def parse_args_blender() -> argparse.Namespace:
    """Parse command line arguments when running inside Blender."""
    # Extract arguments after "--" (Blender passes script args this way)
    argv = sys.argv
    if "--" in argv:
        argv = argv[argv.index("--") + 1:]
    else:
        argv = []

    parser = argparse.ArgumentParser(
        description="Validate Blender font rendering against BLF texture.",
        usage="blender --background --python validate_against_texture.py -- [options] [font_file]",
    )
    parser.add_argument(
        "font_file",
        nargs="?",
        help="Font file to test (.ttf, .otf, .woff, .woff2)",
    )
    parser.add_argument(
        "--font-dir",
        dest="fonts_dir",
        metavar="DIR",
        help="Test all font files in directory",
    )
    parser.add_argument(
        "--chars",
        dest="custom_chars",
        metavar="CHARS",
        help="Test only specific characters (e.g., 'ABC')",
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Show all results (default: only show failures)",
    )
    parser.add_argument(
        "--blend-from-errors",
        dest="blend_from_errors",
        metavar="FILE",
        default="",
        help="Save a .blend file showing failed characters",
    )
    parser.add_argument(
        "--no-fontforge",
        dest="no_fontforge",
        action="store_true",
        help="Don't use FontForge to discover characters, use default charset",
    )
    parser.add_argument(
        "--no-font-sanitize",
        dest="no_font_sanitize",
        action="store_true",
        help="Don't remove overlaps from fonts (skip sanitization)",
    )
    parser.add_argument(
        "--update",
        action="store_true",
        help="Skip rendering if curves and BLF images exist, still run comparison",
    )
    parser.add_argument(
        "--method",
        choices=["FONTFORGE", "SKIA"],
        default="FONTFORGE",
        help="Overlap removal method to use (default: FONTFORGE)",
    )

    args = parser.parse_args(argv)

    # Validate arguments
    if args.fonts_dir is None and args.font_file is None:
        parser.print_help()
        raise ValidationError("No font file or directory specified")

    return args


def get_font_paths(args: argparse.Namespace) -> list[str]:
    """Get list of font paths to test. Raises ValidationError on invalid paths."""
    result: list[str] = []

    # If a specific font file is provided, use only that (no recursive scan)
    # fonts_dir may still be set as the cache base directory
    if args.font_file:
        font_path: str = args.font_file
        if not os.path.isabs(font_path):
            font_path = os.path.abspath(font_path)
        if not os.path.exists(font_path):
            raise ValidationError("Font file not found: {:s}".format(font_path))
        result = [font_path]
    elif args.fonts_dir:
        fonts_dir: str = args.fonts_dir
        if not os.path.isabs(fonts_dir):
            fonts_dir = os.path.abspath(fonts_dir)
        if not os.path.isdir(fonts_dir):
            raise ValidationError("Directory not found: {:s}".format(fonts_dir))
        # Recursively collect all font files, ignoring directories starting with "."
        for root, dirs, files in os.walk(fonts_dir):
            # Filter out directories starting with "." (modifies dirs in-place to skip them)
            dirs[:] = [d for d in dirs if not d.startswith('.')]
            for filename in sorted(files):
                if filename.lower().endswith(FONT_EXTENSIONS):
                    result.append(os.path.join(root, filename))
        result.sort()  # Sort all results for consistent ordering
        if not result:
            raise ValidationError("No font files found in: {:s}".format(fonts_dir))

    return result


def get_fontforge_all_chars(font_path: str) -> str | None:
    """
    Use FontForge to discover all characters in the font.
    Returns a string of all unicode characters that have glyphs.
    """
    if not HAS_FONTFORGE:
        report("Warning: fontforge module not available, using default character set")
        return None

    chars: list[str] = []
    ff_font = fontforge.open(font_path)  # type: ignore[attr-defined]
    for glyph in ff_font.glyphs():
        if glyph.isWorthOutputting() and glyph.unicode > 0:
            try:
                char = chr(glyph.unicode)
                # Skip control characters and other non-printable chars
                if char.isprintable() and not char.isspace():
                    chars.append(char)
            except (ValueError, OverflowError):
                pass
    ff_font.close()
    return "".join(chars)


def get_test_chars(font_path: str, custom_chars: str | None, no_fontforge: bool = False) -> str:
    """Get the characters to test for a given font."""
    if custom_chars:
        return custom_chars
    if no_fontforge:
        return DEFAULT_TEST_CHARS
    return get_fontforge_all_chars(font_path) or DEFAULT_TEST_CHARS


def create_char_curve(font: "BlenderFont", char: str) -> "BlenderObject":
    """
    Create a curve object from a character using the given font.
    Returns the curve object. Caller is responsible for cleanup.
    """
    bpy.ops.object.text_add()
    text_obj = bpy.context.active_object
    text_obj.data.font = font
    text_obj.data.body = char
    bpy.ops.object.convert(target='CURVE')
    return bpy.context.active_object


def render_char_to_image(font: "BlenderFont", char: str, output_path: str, font_path: str) -> None:
    """
    Render a character to an image using Workbench with orthographic camera.
    Camera is scaled to match BLF rendering by comparing blf.dimensions to curve bounds.
    """
    import blf

    # Delete all objects in scene
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)

    # Create curve from character
    curve_obj = create_char_curve(font, char)

    # Create and assign black material (set both node color and viewport color for Workbench)
    mat = bpy.data.materials.new(name="Black")
    mat.diffuse_color = (0.0, 0.0, 0.0, 1.0)  # Viewport/Workbench color
    curve_obj.data.materials.append(mat)

    # Get bounding box of the curve
    bbox_corners = [curve_obj.matrix_world @ mathutils.Vector(corner) for corner in curve_obj.bound_box]
    min_x = min(c.x for c in bbox_corners)
    max_x = max(c.x for c in bbox_corners)
    min_y = min(c.y for c in bbox_corners)
    max_y = max(c.y for c in bbox_corners)

    curve_height = max_y - min_y
    center_x = (min_x + max_x) / 2
    center_y = (min_y + max_y) / 2

    # Get BLF dimensions at a reference size to find the scale ratio
    font_id = blf.load(font_path)
    blf.size(font_id, 100)
    blf_width, blf_height = blf.dimensions(font_id, char)

    # Get BLF em-height for the scaling calculation
    blf_ascender = blf.ascender(font_id)
    blf_descender = -blf.descender(font_id)
    em_height_100 = blf_ascender + blf_descender

    # BLF will render at a size that fits em_height in available_size
    available_pixels = RASTER_OUTPUT_SIZE - (2 * BLF_MARGIN_PX)

    # What size will BLF render the glyph at?
    blf_render_height = blf_height / em_height_100 * available_pixels

    # Set ortho_scale so curve renders at the same pixel size
    # render_pixels = curve_blender_units * (RASTER_OUTPUT_SIZE / ortho_scale)
    # So: ortho_scale = curve_height * RASTER_OUTPUT_SIZE / blf_render_height
    ortho_scale = curve_height * RASTER_OUTPUT_SIZE / blf_render_height if blf_render_height > 0 else 1.0

    # Create orthographic camera looking down -Z axis, centered on the glyph
    bpy.ops.object.camera_add(location=(center_x, center_y, 10))
    camera = bpy.context.active_object
    camera.data.type = 'ORTHO'
    camera.data.ortho_scale = ortho_scale if ortho_scale > 0 else 1.0
    camera.rotation_euler = (0, 0, 0)  # Looking down -Z

    # Set as active camera
    bpy.context.scene.camera = camera

    # Set up Workbench rendering (lightweight, suitable for flat shapes)
    bpy.context.scene.render.engine = 'BLENDER_WORKBENCH'
    bpy.context.scene.render.resolution_x = RASTER_OUTPUT_SIZE
    bpy.context.scene.render.resolution_y = RASTER_OUTPUT_SIZE
    bpy.context.scene.render.film_transparent = True

    # Configure Workbench for flat shading with 8x oversampling
    bpy.context.scene.display.shading.light = 'FLAT'
    bpy.context.scene.display.shading.color_type = 'MATERIAL'
    bpy.context.scene.display.render_aa = '8'

    # Set output path
    bpy.context.scene.render.filepath = output_path
    bpy.context.scene.render.image_settings.file_format = 'PNG'

    # Render
    bpy.ops.render.render(write_still=True)

    # Zealous crop the rendered image
    import imbuf
    ibuf = imbuf.load(output_path)
    crop_rect = ibuf.zealous_crop_rect()
    if crop_rect:
        ibuf.crop(crop_rect[0], crop_rect[1])
    imbuf.write(ibuf, filepath=output_path)


def render_char_to_image_blf(font_path: str, char: str, output_path: str, image_size: int = RASTER_OUTPUT_SIZE) -> None:
    """
    Render a character to an image using BLF (Blender's font drawing).
    Character is centered with BLF_MARGIN_PX margin on all sides.
    """
    import blf
    import imbuf

    ibuf = imbuf.new((image_size, image_size))

    font_id = blf.load(font_path)

    # Black text on transparent background
    blf.color(font_id, 0.0, 0.0, 0.0, 1.0)

    # Available space after margins
    available_size = image_size - (2 * BLF_MARGIN_PX)

    # Use ascender + descender as the consistent height (matching EEVEE approach)
    blf.size(font_id, 100)
    ascender_100 = blf.ascender(font_id)
    descender_100 = -blf.descender(font_id)  # Make positive
    em_height_100 = ascender_100 + descender_100

    # Scale font to fit em_height within available space
    font_size = available_size / em_height_100 * 100
    blf.size(font_id, font_size)
    width, _ = blf.dimensions(font_id, char)

    # Get metrics at final font size
    ascender = blf.ascender(font_id)
    descender = -blf.descender(font_id)

    # Position: center horizontally
    # Baseline Y: center of image, offset so em-square is centered
    # Em-square center is at baseline + (ascender - descender) / 2
    x = (image_size - width) / 2
    y = image_size / 2 - (ascender - descender) / 2

    blf.position(font_id, x, y, 0)

    with blf.bind_imbuf(font_id, ibuf, display_name="sRGB"):
        blf.draw_buffer(font_id, char)

    crop_rect = ibuf.zealous_crop_rect()
    if crop_rect:
        ibuf.crop(crop_rect[0], crop_rect[1])

    imbuf.write(ibuf, filepath=output_path)


def compare_images_idiff(image_a: str, image_b: str, diff_output: str) -> tuple[bool, str]:
    """
    Compare two images using idiff.
    Returns (passed, message) where message contains comparison details.
    Also generates a difference image at diff_output.
    """
    import subprocess

    try:
        result = subprocess.run(
            # failpercent set to 10 to accommodate narrow characters (I, l, 1, etc.)
            # which have a higher ratio of edge/anti-aliased pixels to total pixels
            ["idiff", "-a", "-fail", "0.05", "-failpercent", "10",
             "-o", diff_output, "-abs", image_a, image_b],
            capture_output=True,
            text=True,
        )
        # idiff returns: 0=OK, 1=WARNING (acceptable), 2+=FAILURE
        if result.returncode <= 1:
            return True, "MATCH" if result.returncode == 0 else "WARNING"
        else:
            # Extract useful info from output
            output = result.stdout.strip() or result.stderr.strip()
            return False, output
    except FileNotFoundError:
        return False, "idiff not found"
    except Exception as e:
        return False, str(e)


def test_character(
    font: "BlenderFont", char: str, output_dir: str, font_path: str,
    generated_images: list[CharImageData], update: bool,
) -> tuple[bool, str | None]:
    """Test a single character. Returns (success, error_message).
    Appends (char, curves_filename, blf_filename, diff_filename, passed) to generated_images.
    If update=True, skip rendering when curves and BLF images already exist.
    """
    # Generate filename based on Unicode codepoint
    codepoint = ord(char)

    # Render curves version
    curves_filename = "U{:08X}_curves.png".format(codepoint)
    curves_path = os.path.join(output_dir, curves_filename)

    # Render BLF version
    blf_filename = "U{:08X}_blf.png".format(codepoint)
    blf_path = os.path.join(output_dir, blf_filename)

    # Diff image path
    diff_filename = "U{:08X}_diff.png".format(codepoint)
    diff_path = os.path.join(output_dir, diff_filename)

    # Skip rendering and comparison if both images exist and update mode is enabled.
    if update and os.path.exists(curves_path) and os.path.exists(blf_path):
        # Assume test passed, skip idiff for speed.
        passed = True
    else:
        try:
            render_char_to_image(font, char, curves_path, font_path)
            render_char_to_image_blf(font_path, char, blf_path)
        except Exception as e:
            return False, str(e)

        # Scale curves image to match BLF image size.
        import imbuf
        blf_ibuf = imbuf.load(blf_path)
        curves_ibuf = imbuf.load(curves_path)
        if curves_ibuf.size != blf_ibuf.size:
            curves_ibuf.resize(blf_ibuf.size)
            imbuf.write(curves_ibuf, filepath=curves_path)

        # Compare images using idiff (also generates diff image).
        passed, message = compare_images_idiff(curves_path, blf_path, diff_path)
        if not passed:
            return False, message

    # Track generated images for HTML index (with pass/fail status).
    generated_images.append(CharImageData(char, curves_filename, blf_filename, diff_filename, passed))

    return True, None


def generate_html_index(
    html_dir: str, all_font_data: list[FontHtmlData], no_font_sanitize: bool
) -> None:
    """Generate a single index.html file showing all fonts and their rendered images."""
    html_path = os.path.join(html_dir, "index.html")

    title = "Font Render Comparison"
    if no_font_sanitize:
        title += " (Font Sanitizing Disabled)"

    with open(html_path, "w", encoding="utf-8") as f:
        f.write("""<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <title>{title}</title>
    <style>
        body {{ font-family: sans-serif; margin: 20px; }}
        h1 {{ margin-bottom: 20px; }}
        h2 {{ margin-top: 40px; margin-bottom: 10px; border-bottom: 2px solid #333; padding-bottom: 5px; }}
        .font-path {{ color: #666; font-weight: normal; }}
        table {{ border-collapse: collapse; margin-bottom: 30px; }}
        th, td {{ border: 1px solid #ccc; padding: 8px; text-align: center; vertical-align: middle; }}
        th {{ background: #f0f0f0; }}
        img {{ max-width: 200px; max-height: 200px; }}
        .char {{ font-size: 24px; }}
        .fail {{ background: #ffcccc; }}
    </style>
</head>
<body>
    <h1>{title}</h1>
""".format(title=title))

        for font_data in all_font_data:
            f.write("""    <h2>{font_name} <small class="font-path">({font_relpath})</small></h2>
    <table>
        <tr>
            <th>Char</th>
            <th>Curves</th>
            <th>BLF</th>
            <th>Diff</th>
        </tr>
""".format(font_name=font_data.font_name, font_relpath=font_data.font_relpath))

            # Compute relative path from html_dir to font's output_dir
            try:
                img_prefix = os.path.relpath(font_data.output_dir, html_dir)
            except ValueError:
                img_prefix = font_data.output_dir

            for char, curves_filename, blf_filename, diff_filename, passed in font_data.images:
                codepoint = ord(char)
                row_class = "" if passed else ' class="fail"'
                curves_src = os.path.join(img_prefix, curves_filename)
                blf_src = os.path.join(img_prefix, blf_filename)
                diff_src = os.path.join(img_prefix, diff_filename)
                f.write("""        <tr{row_class}>
            <td class="char">{char}<br><small>U+{codepoint:04X}</small></td>
            <td><img src="{curves_src}" alt="curves"></td>
            <td><img src="{blf_src}" alt="blf"></td>
            <td><img src="{diff_src}" alt="diff"></td>
        </tr>
""".format(row_class=row_class, char=char, codepoint=codepoint, curves_src=curves_src, blf_src=blf_src, diff_src=diff_src))

            f.write("""    </table>
""")

        f.write("""</body>
</html>
""")


def test_font(
    font_path: str, custom_chars: str | None, verbose: bool, base_dir: str | None = None,
    no_fontforge: bool = False, no_font_sanitize: bool = False, update: bool = False,
    method: str = "FONTFORGE",
) -> FontTestResult:
    """Test all characters in the font. Returns FontTestResult."""
    test_chars = get_test_chars(font_path, custom_chars, no_fontforge)

    report("\nTesting font: {:s}".format(font_path))
    report("Testing {:d} characters\n".format(len(test_chars)))

    # Determine output directory for rendered images
    font_name = os.path.splitext(os.path.basename(font_path))[0]
    font_basename = os.path.basename(font_path)
    if base_dir is not None:
        # Compute relative path from base_dir to font's directory
        font_dir = os.path.dirname(font_path)
        try:
            rel_path = os.path.relpath(font_dir, base_dir)
        except ValueError:
            rel_path = ""
        if rel_path == ".":
            rel_path = ""
        output_dir = os.path.join(base_dir, ".raster_fonts", rel_path, font_name)
        # font_relpath is the path to the font file relative to base_dir
        font_relpath = os.path.join(rel_path, font_basename) if rel_path else font_basename
    else:
        output_dir = os.path.join(os.path.dirname(font_path), ".raster_fonts", font_name)
        font_relpath = font_basename
    os.makedirs(output_dir, exist_ok=True)

    # Load fonts once
    bpy.ops.wm.read_factory_settings(use_empty=True)

    font: "BlenderFont" = bpy.data.fonts.load(font_path)
    if not no_font_sanitize:
        font.use_overlap_removal = True
        font.overlap_removal_method = method

    passed_count = 0
    failed: list[CharTestResult] = []
    generated_images: list[CharImageData] = []
    total_chars = len(test_chars)
    num_width = len(str(total_chars))

    for i, char in enumerate(test_chars, 1):
        report("  {:0{width}d} of {:d} [{:s}] ".format(i, total_chars, char, width=num_width), end="")
        success, error = test_character(font, char, output_dir, font_path, generated_images, update)
        if success:
            passed_count += 1
            report("PASS" if verbose else "")
        else:
            failed.append(CharTestResult(char=char, error=error))
            report("FAIL: {:s}".format(str(error)))

    # Summary
    try:
        font_display = os.path.relpath(font_path)
    except ValueError:
        # relpath can fail on Windows with different drives
        font_display = font_path
    report("\n{:s}".format(SEPARATOR_SHORT))
    report("RESULTS: {:d} passed, {:d} failed ({:s})".format(passed_count, len(failed), font_display))
    report("{:s}".format(SEPARATOR_SHORT))

    if failed:
        report("\nFailed characters:")
        for result in failed:
            report("  '{:s}': {:s}".format(result.char, str(result.error)))

    # Build HTML data for this font
    html_data: FontHtmlData | None = None
    if generated_images:
        html_data = FontHtmlData(
            font_name=font_name,
            font_relpath=font_relpath,
            output_dir=output_dir,
            images=generated_images,
        )

    return FontTestResult(passed=passed_count, failed=len(failed), failed_chars=failed, html_data=html_data)


def save_error_blend(
    blend_path: str, failed_fonts: list[tuple[str, list[CharTestResult]]]
) -> None:
    """
    Save a .blend file with text objects for each failed character.
    Each text object is positioned in a line along the X axis.
    Includes a text data-block with the full validation report.
    """
    # Clear the scene
    bpy.ops.wm.read_factory_settings(use_empty=True)

    x_pos = 0.0
    for font_path, failed_chars in failed_fonts:
        font: "BlenderFont" = bpy.data.fonts.load(font_path)

        # Create a text object for each failed character
        for result in failed_chars:
            bpy.ops.object.text_add(location=(x_pos, 0.0, 0.0))
            text_obj = bpy.context.active_object
            text_obj.data.font = font
            text_obj.data.body = result.char
            text_obj.name = "{:s}_{:s}".format(
                os.path.splitext(os.path.basename(font_path))[0], result.char
            )
            x_pos += 1.0

    # Create a text data-block with the validation report
    report_text = bpy.data.texts.new("validation_report.txt")
    report_text.from_string("\n".join(_report_log))

    # Save the blend file
    bpy.ops.wm.save_as_mainfile(filepath=blend_path)
    report("Saved error blend file: {:s}".format(blend_path))


def main_blender() -> int:
    """Main entry point when running inside Blender."""
    try:
        args = parse_args_blender()
        font_paths = get_font_paths(args)
    except ValidationError as e:
        report("Error: {:s}".format(str(e)))
        return 1

    # Determine base_dir for output paths (only when using --font-dir)
    base_dir: str | None = None
    if args.fonts_dir:
        base_dir = args.fonts_dir
        if not os.path.isabs(base_dir):
            base_dir = os.path.abspath(base_dir)

    total_passed = 0
    total_failed = 0
    all_failed: list[tuple[str, list[CharTestResult]]] = []
    all_html_data: list[FontHtmlData] = []

    for font_path in font_paths:
        result = test_font(
            font_path,
            args.custom_chars,
            args.verbose,
            base_dir,
            args.no_fontforge,
            args.no_font_sanitize,
            args.update,
            args.method)
        total_passed += result.passed
        total_failed += result.failed
        if result.failed_chars:
            all_failed.append((font_path, result.failed_chars))
        if result.html_data:
            all_html_data.append(result.html_data)

    # Generate combined HTML index
    if all_html_data:
        all_html_data.sort(key=lambda x: x.font_relpath)
        if base_dir is not None:
            html_dir = os.path.join(base_dir, ".raster_fonts")
        else:
            # Use the parent of the first font's output_dir
            html_dir = os.path.dirname(all_html_data[0].output_dir)
        os.makedirs(html_dir, exist_ok=True)
        generate_html_index(html_dir, all_html_data, args.no_font_sanitize)
        report("\nGenerated HTML index: {:s}".format(os.path.join(html_dir, "index.html")))

    # Print grand total if multiple fonts were tested
    if len(font_paths) > 1:
        report("\n{:s}".format(SEPARATOR_LONG))
        report("GRAND TOTAL: {:d} passed, {:d} failed across {:d} fonts".format(
            total_passed, total_failed, len(font_paths)
        ))
        report("{:s}".format(SEPARATOR_LONG))

        if all_failed:
            report("\nFailed fonts:")
            for font_path, failed_chars in all_failed:
                font_name = os.path.basename(font_path)
                chars = ", ".join("'{:s}'".format(r.char) for r in failed_chars[:5])
                if len(failed_chars) > 5:
                    chars += ", ... ({:d} total)".format(len(failed_chars))
                report("  {:s}: {:s}".format(font_name, chars))

    # Save error blend file if requested
    if args.blend_from_errors and all_failed:
        save_error_blend(args.blend_from_errors, all_failed)

    return 0 if total_failed == 0 else 1


def main() -> int:
    """Main entry point - dispatches based on execution context."""
    if INSIDE_BLENDER:
        return main_blender()
    else:
        return main_direct()


if __name__ == "__main__":
    sys.exit(main())
