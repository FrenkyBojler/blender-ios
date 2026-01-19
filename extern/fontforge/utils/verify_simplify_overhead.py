#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2024 Blender Authors
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Measure the overhead of simplify/overlap removal options for font glyphs.

This script creates a 3D text object with all glyphs from a font and times
the evaluation with different simplify options.

Since font glyph evaluation is cached after the first evaluation, this script
runs multiple fresh sessions (one per sample) to measure uncached evaluation time.

Usage:
    blender --background --python verify_simplify_overhead.py -- /path/to/font.ttf

Options:
    --samples N       Number of fresh sessions to run for each test (default: 3)
    --text TEXT       Custom text to use instead of all glyphs
    --verbose         Print detailed timing information
    --mode MODE       Benchmark mode:
                      - curve_simplify: Test curve's use_simplify_fill (Skia before scan-fill)
                      - font_simplify: Compare Skia vs FontForge overlap removal methods
"""

import argparse
import sys
import time
from pathlib import Path


def get_font_glyphs(font_path: str) -> str:
    """
    Get a string containing representative glyphs to test.
    Uses common character ranges that most fonts support.
    """
    chars = []

    # Basic Latin (ASCII printable)
    chars.extend(chr(c) for c in range(0x20, 0x7F))

    # Latin-1 Supplement
    chars.extend(chr(c) for c in range(0xA0, 0x100))

    # Latin Extended-A (subset)
    chars.extend(chr(c) for c in range(0x100, 0x180))

    # Common punctuation and symbols
    chars.extend(chr(c) for c in range(0x2000, 0x2070))

    # Filter to only include characters that can be displayed
    # (exclude control characters and combining marks that might cause issues)
    filtered = []
    for c in chars:
        if c.isprintable() and not c.isspace():
            filtered.append(c)

    # Add newlines periodically to create multiple lines
    result = []
    chars_per_line = 40
    for i, c in enumerate(filtered):
        result.append(c)
        if (i + 1) % chars_per_line == 0:
            result.append('\n')

    return ''.join(result)


def force_evaluation(depsgraph, obj):
    """Force evaluation of the object."""
    # Update the dependency graph
    depsgraph.update()
    # Access evaluated object to ensure computation
    obj_eval = obj.evaluated_get(depsgraph)
    # Access the mesh to force full evaluation
    if obj_eval.data:
        _ = obj_eval.to_mesh()
        obj_eval.to_mesh_clear()


def create_test_text_object(
    context,
    font_path: str,
    text: str,
    use_font_simplify: bool = False,
    simplify_method: str | None = None,
):
    """Create a text object with the specified font and text."""
    import bpy

    # Load the font
    font = bpy.data.fonts.load(font_path)

    # Set font simplify mode BEFORE assigning to curve
    # (overlap removal happens during glyph loading)
    if use_font_simplify and simplify_method:
        font.use_simplify = True
        font.simplify_method = simplify_method
    else:
        font.use_simplify = False

    # Create a new text curve
    curve_data = bpy.data.curves.new(name="TestText", type='FONT')
    curve_data.font = font  # Assign font first
    curve_data.body = text  # Then set text (triggers glyph loading)

    # Set to 2D so fill is used
    curve_data.dimensions = '2D'
    curve_data.fill_mode = 'BOTH'

    # Create the object
    obj = bpy.data.objects.new(name="TestTextObject", object_data=curve_data)

    # Link to scene
    context.collection.objects.link(obj)

    return obj, font


def run_single_test(
    font_path: str,
    text: str,
    use_simplify: bool,
    simplify_method: str | None = None,
    use_font_simplify: bool = False,
) -> float:
    """
    Run a single test with fresh factory settings to avoid glyph caching.
    Returns the evaluation time for the first (uncached) evaluation.

    Args:
        font_path: Path to the font file.
        text: Text to render.
        use_simplify: Enable curve's use_simplify_fill (Skia before scan-fill).
        simplify_method: For font simplify, method to use ('SKIA' or 'FONTFORGE').
        use_font_simplify: Enable font's use_simplify (overlap removal during glyph loading).
    """
    import bpy

    # Start fresh to avoid any cached data
    bpy.ops.wm.read_factory_settings(use_empty=True)

    context = bpy.context
    depsgraph = context.evaluated_depsgraph_get()

    # Create the text object (font simplify settings applied during creation)
    obj, font = create_test_text_object(
        context, font_path, text,
        use_font_simplify=use_font_simplify,
        simplify_method=simplify_method,
    )
    curve = obj.data

    # Set curve simplify mode (Skia before scan-fill)
    curve.use_simplify_fill = use_simplify

    # Time the first (uncached) evaluation
    start = time.perf_counter()
    force_evaluation(depsgraph, obj)
    end = time.perf_counter()

    return end - start


def run_benchmark(
    font_path: str,
    samples: int = 3,
    custom_text: str | None = None,
    verbose: bool = False,
    mode: str = "curve_simplify",
):
    """
    Run the benchmark comparing evaluation with and without simplify.

    Since glyph evaluation is cached after the first evaluation, we run
    multiple fresh sessions (one per sample) to measure uncached evaluation time.

    Args:
        mode: Benchmark mode:
            - "curve_simplify": Test curve's use_simplify_fill (Skia before scan-fill)
            - "font_simplify": Test font's use_simplify comparing SKIA vs FONTFORGE methods
    """
    # Determine text to use
    if custom_text:
        text = custom_text
    else:
        text = get_font_glyphs(font_path)

    print(f"Font: {font_path}")
    print(f"Text length: {len(text)} characters")
    print(f"Samples: {samples} (fresh sessions per test)")
    print(f"Mode: {mode}")
    print()

    if mode == "curve_simplify":
        # Test curve's use_simplify_fill (Skia before scan-fill)
        times_without = []
        times_with = []

        for i in range(samples):
            print(f"Sample {i + 1}/{samples}: Testing WITHOUT curve simplify fill...")
            t = run_single_test(font_path, text, use_simplify=False)
            times_without.append(t)
            if verbose:
                print(f"  Time: {t:.4f}s")

            print(f"Sample {i + 1}/{samples}: Testing WITH curve simplify fill (Skia)...")
            t = run_single_test(font_path, text, use_simplify=True)
            times_with.append(t)
            if verbose:
                print(f"  Time: {t:.4f}s")

        _print_comparison_results(
            "Without curve simplify", times_without,
            "With curve simplify (Skia)", times_with,
            verbose
        )

    elif mode == "font_simplify":
        # Test font's use_simplify comparing methods
        times_none = []
        times_skia = []
        times_fontforge = []

        # Warmup run (discarded) to avoid first-run overhead affecting results
        print("Warmup run (discarded)...")
        run_single_test(font_path, text, use_simplify=False, use_font_simplify=False)

        for i in range(samples):
            print(f"Sample {i + 1}/{samples}: Testing WITHOUT font simplify...")
            t = run_single_test(font_path, text, use_simplify=False, use_font_simplify=False)
            times_none.append(t)
            if verbose:
                print(f"  Time: {t:.4f}s")

            print(f"Sample {i + 1}/{samples}: Testing WITH font simplify (Skia)...")
            t = run_single_test(font_path, text, use_simplify=False,
                               use_font_simplify=True, simplify_method='SKIA')
            times_skia.append(t)
            if verbose:
                print(f"  Time: {t:.4f}s")

            print(f"Sample {i + 1}/{samples}: Testing WITH font simplify (FontForge)...")
            t = run_single_test(font_path, text, use_simplify=False,
                               use_font_simplify=True, simplify_method='FONTFORGE')
            times_fontforge.append(t)
            if verbose:
                print(f"  Time: {t:.4f}s")

        _print_three_way_results(
            "No simplify", times_none,
            "Skia", times_skia,
            "FontForge", times_fontforge,
            verbose
        )


def _print_comparison_results(label1, times1, label2, times2, verbose):
    """Print comparison results for two test configurations."""
    avg1 = sum(times1) / len(times1)
    avg2 = sum(times2) / len(times2)

    overhead = avg2 - avg1
    overhead_percent = (overhead / avg1) * 100 if avg1 > 0 else 0

    print("\n" + "=" * 60)
    print("RESULTS")
    print("=" * 60)

    if verbose:
        print(f"\n{label1} (times): {[f'{t:.4f}s' for t in times1]}")
        print(f"{label2} (times): {[f'{t:.4f}s' for t in times2]}")

    print(f"\n{label1}:")
    print(f"  Average: {avg1:.4f}s")
    print(f"  Min:     {min(times1):.4f}s")
    print(f"  Max:     {max(times1):.4f}s")

    print(f"\n{label2}:")
    print(f"  Average: {avg2:.4f}s")
    print(f"  Min:     {min(times2):.4f}s")
    print(f"  Max:     {max(times2):.4f}s")

    print(f"\nOverhead:")
    print(f"  Absolute: {overhead:+.4f}s")
    print(f"  Relative: {overhead_percent:+.1f}%")

    print("=" * 60)


def _print_three_way_results(label1, times1, label2, times2, label3, times3, verbose):
    """Print comparison results for three test configurations."""
    avg1 = sum(times1) / len(times1)
    avg2 = sum(times2) / len(times2)
    avg3 = sum(times3) / len(times3)

    overhead2 = avg2 - avg1
    overhead2_percent = (overhead2 / avg1) * 100 if avg1 > 0 else 0
    overhead3 = avg3 - avg1
    overhead3_percent = (overhead3 / avg1) * 100 if avg1 > 0 else 0

    print("\n" + "=" * 60)
    print("RESULTS")
    print("=" * 60)

    if verbose:
        print(f"\n{label1} (times): {[f'{t:.4f}s' for t in times1]}")
        print(f"{label2} (times): {[f'{t:.4f}s' for t in times2]}")
        print(f"{label3} (times): {[f'{t:.4f}s' for t in times3]}")

    print(f"\n{label1}:")
    print(f"  Average: {avg1:.4f}s")
    print(f"  Min:     {min(times1):.4f}s")
    print(f"  Max:     {max(times1):.4f}s")

    print(f"\n{label2}:")
    print(f"  Average: {avg2:.4f}s")
    print(f"  Min:     {min(times2):.4f}s")
    print(f"  Max:     {max(times2):.4f}s")
    print(f"  Overhead vs {label1}: {overhead2:+.4f}s ({overhead2_percent:+.1f}%)")

    print(f"\n{label3}:")
    print(f"  Average: {avg3:.4f}s")
    print(f"  Min:     {min(times3):.4f}s")
    print(f"  Max:     {max(times3):.4f}s")
    print(f"  Overhead vs {label1}: {overhead3:+.4f}s ({overhead3_percent:+.1f}%)")

    # Compare the two simplify methods
    if avg2 < avg3:
        faster = label2
        slower = label3
        diff = avg3 - avg2
    else:
        faster = label3
        slower = label2
        diff = avg2 - avg3

    diff_percent = (diff / min(avg2, avg3)) * 100 if min(avg2, avg3) > 0 else 0
    print(f"\n{faster} is {diff:.4f}s ({diff_percent:.1f}%) faster than {slower}")

    print("=" * 60)


def main():
    # Parse arguments after "--"
    argv = sys.argv
    if "--" in argv:
        argv = argv[argv.index("--") + 1:]
    else:
        argv = []

    parser = argparse.ArgumentParser(
        description="Measure simplify fill overhead for font evaluation"
    )
    parser.add_argument(
        "font_path",
        type=str,
        help="Path to the font file (.ttf, .otf, etc.)",
    )
    parser.add_argument(
        "--samples",
        type=int,
        default=3,
        help="Number of fresh sessions to run for each test (default: 3)",
    )
    parser.add_argument(
        "--text",
        type=str,
        default=None,
        help="Custom text to use instead of all glyphs",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Print detailed timing information",
    )
    parser.add_argument(
        "--mode",
        type=str,
        choices=["curve_simplify", "font_simplify"],
        default="curve_simplify",
        help="Benchmark mode: 'curve_simplify' tests Skia before scan-fill, "
             "'font_simplify' compares Skia vs FontForge overlap removal (default: curve_simplify)",
    )

    args = parser.parse_args(argv)

    # Validate font path
    font_path = Path(args.font_path)
    if not font_path.exists():
        print(f"Error: Font file not found: {font_path}", file=sys.stderr)
        sys.exit(1)

    # Run the benchmark
    run_benchmark(
        font_path=str(font_path.resolve()),
        samples=args.samples,
        custom_text=args.text,
        verbose=args.verbose,
        mode=args.mode,
    )


if __name__ == "__main__":
    main()
