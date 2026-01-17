#!/usr/bin/env python3
"""
Validation script for Blender font overlap removal.

Compares Blender's overlap removal against FontForge's implementation by loading
fonts and comparing the resulting curve statistics (area, perimeter, spline count).

Can be run directly or through Blender:
    ./validate.py                       # Test all fonts in ./fonts (auto-finds Blender)
    BLENDER_JOBS=4 ./validate.py        # Run with 4 parallel jobs
    ./validate.py path/to/font.ttf      # Test specific font
    blender --background --python validate.py -- [options]
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

# Optional fontforge import - used for generating reference fonts
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
FONT_EXTENSIONS: tuple[str, ...] = ('.ttf', '.otf', '.woff', '.woff2')

# Cache directory names for generated fonts (start with . to be ignored in recursive search)
CLEAN_CACHE_DIR: str = ".clean_cache"
DIRTY_CACHE_DIR: str = ".dirty_cache"

# Global log for report messages
_report_log: list[str] = []

# Number of samples for bezier curve approximation
BEZIER_SAMPLES: int = 20

# Tolerance for area comparison (relative difference)
AREA_TOLERANCE: float = 0.03  # 3% difference allowed
# Tolerance for perimeter comparison (relative difference)
# Perimeter is more sensitive to curve conversion (quadratic to cubic) differences
PERIMETER_TOLERANCE: float = 0.15  # 15% difference allowed for perimeter
# Minimum area for percentage comparison (below this, use absolute comparison)
AREA_MIN_THRESHOLD: float = 0.05
# Minimum perimeter for percentage comparison
PERIMETER_MIN_THRESHOLD: float = 0.01

# Output formatting
SEPARATOR_SHORT: str = "=" * 50
SEPARATOR_LONG: str = "=" * 60

DEFAULT_TEST_CHARS: str = (
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789"
    "!@#$%^&*()_+-=[]{}|;':\",./<>?`~"
)


class CurveStats(NamedTuple):
    """Statistics for a curve object."""
    area: float
    perimeter: float
    spline_count: int


class CharTestResult(NamedTuple):
    """Result of testing a single character."""
    char: str
    error: str | None


class FontTestResult(NamedTuple):
    """Result of testing a font."""
    passed: int
    failed: int
    failed_chars: list[CharTestResult]


class JobResult(NamedTuple):
    """Result of a parallel job."""
    font: str
    returncode: int
    stdout: str
    stderr: str


class ValidationError(Exception):
    """Raised when validation setup fails."""


def report(message: str = "") -> None:
    """Print message to stdout and append to global log."""
    print(message)
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

    with ThreadPoolExecutor(max_workers=jobs) as executor:
        futures = {
            executor.submit(run_single_font_job, blender, validate_py, font, extra_args): font
            for font in fonts
        }

        for future in as_completed(futures):
            result = future.result()
            # Print output immediately as each job finishes
            if result.stdout:
                print(result.stdout, end="", flush=True)
            if result.stderr:
                print(result.stderr, end="", file=sys.stderr, flush=True)
            if result.returncode != 0:
                max_returncode = max(max_returncode, result.returncode)

    return max_returncode


def print_direct_help() -> None:
    """Print help message for direct execution mode."""
    print("""\
Validation script for Blender font overlap removal.

This script can be run directly or through Blender. When run directly,
it spawns Blender subprocess(es) and forwards all arguments.

Usage:
  ./validate.py [options] [font_file]
  blender --background --python validate.py -- [options] [font_file]

Environment variables (direct mode only):
  BLENDER_BIN   Path to Blender executable (default: "blender")
  BLENDER_JOBS  Number of parallel jobs (default: 1, 0 = number of CPU cores)
              When > 1, runs one Blender process per font file.

Options (passed to Blender):
  font_file               Font file to test (.ttf, .otf, .woff, .woff2)
  --font-dir DIR          Test all font files in directory
  --chars CHARS           Test only specific characters (e.g., 'ABC')
  -v, --verbose           Show all results (default: only show failures)
  --blend-from-errors FILE
                          Save a .blend file showing failed characters
  -h, --help              Show this help message

Examples:
  ./validate.py                              Test all fonts in ./fonts
  BLENDER_JOBS=0 ./validate.py               Run with all CPU cores
  BLENDER_JOBS=4 ./validate.py               Run with 4 parallel jobs
  ./validate.py --chars 'ABC'                Test only specific characters
  ./validate.py path/to/font.ttf             Test a specific font
  BLENDER_BIN=/path/to/blender ./validate.py Use specific Blender build
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
        description="Validate Blender font overlap removal against FontForge.",
        usage="blender --background --python validate.py -- [options] [font_file]",
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


def get_cached_font_path(font_path: str, cache_subdir: str, base_dir: str | None = None) -> str:
    """
    Get the path to a cached version of a font.

    If base_dir is provided, the cache is stored at base_dir/cache_subdir/relative/path/font.ttf
    where relative/path is the path from base_dir to the font file.

    If base_dir is None, the cache is stored next to the font file.
    """
    font_name = os.path.basename(font_path)
    base_name, _ = os.path.splitext(font_name)

    if base_dir is not None:
        # Compute relative path from base_dir to font's directory
        font_dir = os.path.dirname(font_path)
        try:
            rel_path = os.path.relpath(font_dir, base_dir)
        except ValueError:
            # relpath can fail on Windows with different drives
            rel_path = ""
        # Handle case where font is directly in base_dir (rel_path would be ".")
        if rel_path == ".":
            rel_path = ""
        cache_dir = os.path.join(base_dir, cache_subdir, rel_path)
    else:
        font_dir = os.path.dirname(font_path)
        cache_dir = os.path.join(font_dir, cache_subdir)

    return os.path.join(cache_dir, "{:s}.ttf".format(base_name))


def generate_font(
    source_path: str, output_path: str, remove_overlap: bool
) -> None:
    """
    Generate a font file using FontForge.
    Unlinks references and optionally removes overlaps.
    """
    ff_font = fontforge.open(source_path)  # type: ignore[attr-defined]
    ff_font.selection.all()
    ff_font.unlinkReferences()
    if remove_overlap:
        for glyph in ff_font.glyphs():
            if glyph.isWorthOutputting():
                glyph.removeOverlap()
    ff_font.generate(output_path)
    ff_font.close()


def ensure_normalized_fonts(font_path: str, base_dir: str | None = None) -> tuple[str | None, str | None]:
    """
    Ensure both normalized (no overlap removal) and fixed (with overlap removal)
    versions of the font exist. Both are generated by FontForge to ensure
    consistent font output for fair comparison.

    Returns (normalized_path, fixed_path) or (None, None) if creation failed.
    """
    if not HAS_FONTFORGE:
        report("Warning: fontforge module not available, cannot create fonts")
        return None, None

    fixed_path = get_cached_font_path(font_path, CLEAN_CACHE_DIR, base_dir)
    normalized_path = get_cached_font_path(font_path, DIRTY_CACHE_DIR, base_dir)

    # Check if both fonts are up to date
    source_mtime = os.path.getmtime(font_path)
    fixed_ok = os.path.exists(fixed_path) and os.path.getmtime(fixed_path) >= source_mtime
    normalized_ok = os.path.exists(normalized_path) and os.path.getmtime(normalized_path) >= source_mtime

    if fixed_ok and normalized_ok:
        return normalized_path, fixed_path

    # Ensure cache directories exist
    os.makedirs(os.path.dirname(fixed_path), exist_ok=True)
    os.makedirs(os.path.dirname(normalized_path), exist_ok=True)

    try:
        if not normalized_ok:
            report("  Generating normalized font: {:s}".format(os.path.basename(normalized_path)))
            generate_font(font_path, normalized_path, remove_overlap=False)

        if not fixed_ok:
            report("  Generating fixed font: {:s}".format(os.path.basename(fixed_path)))
            generate_font(font_path, fixed_path, remove_overlap=True)

        return normalized_path, fixed_path
    except Exception as e:
        report("Warning: Failed to create fonts: {:s}".format(str(e)))
        return None, None


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


def get_test_chars(font_path: str, custom_chars: str | None) -> str:
    """Get the characters to test for a given font."""
    return custom_chars or get_fontforge_all_chars(font_path) or DEFAULT_TEST_CHARS


def bezier_segment_stats(
    p0: Any, p1: Any, p2: Any, p3: Any, num_samples: int = BEZIER_SAMPLES
) -> tuple[float, float]:
    """
    Calculate length and signed area of a cubic bezier segment by sampling.
    Returns (length, area) tuple.
    """
    length = 0.0
    area = 0.0
    prev = p0
    prev_x, prev_y = p0.x, p0.y

    for i in range(1, num_samples + 1):
        t = i / num_samples
        mt = 1 - t
        # Cubic bezier point
        x = mt**3 * p0.x + 3 * mt**2 * t * p1.x + 3 * mt * t**2 * p2.x + t**3 * p3.x
        y = mt**3 * p0.y + 3 * mt**2 * t * p1.y + 3 * mt * t**2 * p2.y + t**3 * p3.y
        pt = mathutils.Vector((x, y))
        # Length contribution
        length += (pt - prev).length
        prev = pt
        # Shoelace area contribution
        area += prev_x * y - x * prev_y
        prev_x, prev_y = x, y

    return length, area / 2.0


def get_blender_curve_stats(curve_obj: "BlenderObject") -> CurveStats:
    """
    Get statistics from a Blender curve object.
    Returns CurveStats with area, perimeter, and spline_count.
    """
    if curve_obj.type != 'CURVE':
        return CurveStats(area=0.0, perimeter=0.0, spline_count=0)

    curve = curve_obj.data
    total_area = 0.0
    total_perimeter = 0.0
    spline_count = 0

    for spline in curve.splines:
        if spline.type != 'BEZIER':
            continue

        points = spline.bezier_points
        num_points = len(points)

        # Skip degenerate splines with less than 2 points (not renderable)
        if num_points < 2:
            continue

        spline_count += 1

        # Calculate perimeter and area by iterating over bezier segments
        spline_area = 0.0
        for i in range(num_points):
            if spline.use_cyclic_u or i < num_points - 1:
                next_i = (i + 1) % num_points
                p0 = mathutils.Vector(points[i].co[:2])
                p1 = mathutils.Vector(points[i].handle_right[:2])
                p2 = mathutils.Vector(points[next_i].handle_left[:2])
                p3 = mathutils.Vector(points[next_i].co[:2])
                seg_length, seg_area = bezier_segment_stats(p0, p1, p2, p3)
                total_perimeter += seg_length
                spline_area += seg_area

        total_area += abs(spline_area)

    return CurveStats(area=total_area, perimeter=total_perimeter, spline_count=spline_count)


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


def check_area_difference(
    blender_area: float, ff_area: float
) -> str | None:
    """
    Check if area difference exceeds tolerance.
    Returns error message if difference is too large, None otherwise.
    """
    if ff_area > AREA_MIN_THRESHOLD:
        area_diff = abs(blender_area - ff_area) / ff_area
        if area_diff > AREA_TOLERANCE:
            return "area differs by {:.1f}%: Blender={:.2f}, FontForge={:.2f}".format(
                area_diff * 100, blender_area, ff_area
            )
    elif ff_area > 0:
        abs_diff = abs(blender_area - ff_area)
        if abs_diff > AREA_MIN_THRESHOLD * AREA_TOLERANCE:
            return "area differs: Blender={:.4f}, FontForge={:.4f}".format(
                blender_area, ff_area
            )
    elif blender_area > 0:
        return "area differs: Blender={:.2f}, FontForge={:.2f}".format(
            blender_area, ff_area
        )
    return None


def check_perimeter_difference(
    blender_perimeter: float, ff_perimeter: float
) -> str | None:
    """
    Check if perimeter difference exceeds tolerance.
    Returns error message if difference is too large, None otherwise.
    """
    if ff_perimeter > PERIMETER_MIN_THRESHOLD:
        perimeter_diff = abs(blender_perimeter - ff_perimeter) / ff_perimeter
        if perimeter_diff > PERIMETER_TOLERANCE:
            return "perimeter differs by {:.1f}%: Blender={:.2f}, FontForge={:.2f}".format(
                perimeter_diff * 100, blender_perimeter, ff_perimeter
            )
    return None


def compare_with_fontforge(
    blender_curve_obj: "BlenderObject", fixed_font: "BlenderFont", char: str
) -> tuple[bool, str | None]:
    """
    Compare Blender curve with FontForge-fixed result.
    Compares area, perimeter length, and number of curves.
    Returns (success, error_message) tuple.
    """
    blender_stats = get_blender_curve_stats(blender_curve_obj)

    # Get FontForge stats
    ff_curve = create_char_curve(fixed_font, char)
    ff_stats = get_blender_curve_stats(ff_curve)
    bpy.data.objects.remove(ff_curve, do_unlink=True)

    errors: list[str] = []

    # Compare number of splines (curves) - must match exactly
    if blender_stats.spline_count != ff_stats.spline_count:
        errors.append(
            "spline count differs: Blender={:d}, FontForge={:d}".format(
                blender_stats.spline_count, ff_stats.spline_count
            )
        )

    # Compare areas (with tolerance)
    area_error = check_area_difference(blender_stats.area, ff_stats.area)
    if area_error:
        errors.append(area_error)

    # Compare perimeter length (with tolerance)
    perimeter_error = check_perimeter_difference(blender_stats.perimeter, ff_stats.perimeter)
    if perimeter_error:
        errors.append(perimeter_error)

    if errors:
        return False, "FontForge comparison: " + "; ".join(errors)

    return True, None


def test_character(
    font: "BlenderFont", fixed_font: "BlenderFont | None", char: str
) -> tuple[bool, str | None]:
    """Test a single character. Returns (success, error_message)."""
    # Delete all objects in scene
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)

    # Create curve from character
    curve_obj = create_char_curve(font, char)

    # Compare with FontForge-processed output
    if fixed_font is None:
        # No comparison font available - skip comparison
        return True, None

    return compare_with_fontforge(curve_obj, fixed_font, char)


def test_font(
    font_path: str, custom_chars: str | None, verbose: bool, base_dir: str | None = None
) -> FontTestResult:
    """Test all characters in the font. Returns FontTestResult."""
    test_chars = get_test_chars(font_path, custom_chars)

    report("\nTesting font: {:s}".format(font_path))
    report("Testing {:d} characters\n".format(len(test_chars)))

    # Load fonts once
    bpy.ops.wm.read_factory_settings(use_empty=True)

    # Generate both normalized fonts through FontForge for fair comparison
    normalized_path, fixed_path = ensure_normalized_fonts(font_path, base_dir)
    fixed_font: "BlenderFont | None" = None

    if normalized_path and fixed_path:
        # Load normalized font (no overlap removal by FontForge) with Blender overlap removal
        font: "BlenderFont" = bpy.data.fonts.load(normalized_path)
        font.use_overlap_removal = True

        # Load fixed font (overlap removal by FontForge) without Blender overlap removal
        fixed_font = bpy.data.fonts.load(fixed_path)
        fixed_font.use_overlap_removal = False

        report(
            "Comparing: {:s} (Blender) vs {:s} (FontForge)\n".format(
                os.path.basename(normalized_path), os.path.basename(fixed_path)
            )
        )
    else:
        report("Warning: Could not create normalized fonts, skipping comparison\n")
        font = bpy.data.fonts.load(font_path)
        font.use_overlap_removal = True

    passed_count = 0
    failed: list[CharTestResult] = []

    for char in test_chars:
        success, error = test_character(font, fixed_font, char)
        if success:
            passed_count += 1
            if verbose:
                report("  PASS: '{:s}'".format(char))
        else:
            failed.append(CharTestResult(char=char, error=error))
            report("  FAIL: '{:s}' - {:s}".format(char, str(error)))

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

    return FontTestResult(passed=passed_count, failed=len(failed), failed_chars=failed)


def save_error_blend(
    blend_path: str, failed_fonts: list[tuple[str, list[CharTestResult]]], base_dir: str | None = None
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
        # Load the font (use dirty_cache version with overlap removal enabled)
        normalized_path = get_cached_font_path(font_path, DIRTY_CACHE_DIR, base_dir)
        if os.path.exists(normalized_path):
            font: "BlenderFont" = bpy.data.fonts.load(normalized_path)
        else:
            font = bpy.data.fonts.load(font_path)
        font.use_overlap_removal = True

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

    # Determine base_dir for cache paths (only when using --font-dir)
    base_dir: str | None = None
    if args.fonts_dir:
        base_dir = args.fonts_dir
        if not os.path.isabs(base_dir):
            base_dir = os.path.abspath(base_dir)

    total_passed = 0
    total_failed = 0
    all_failed: list[tuple[str, list[CharTestResult]]] = []

    for font_path in font_paths:
        result = test_font(font_path, args.custom_chars, args.verbose, base_dir)
        total_passed += result.passed
        total_failed += result.failed
        if result.failed_chars:
            all_failed.append((font_path, result.failed_chars))

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
        save_error_blend(args.blend_from_errors, all_failed, base_dir)

    return 0 if total_failed == 0 else 1


def main() -> int:
    """Main entry point - dispatches based on execution context."""
    if INSIDE_BLENDER:
        return main_blender()
    else:
        return main_direct()


if __name__ == "__main__":
    sys.exit(main())
