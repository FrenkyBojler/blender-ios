#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2024 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Generate an HTML page showing side-by-side comparison of legacy vs non-legacy SVG outputs.

Scans *.json files in the data/ directory and generates an HTML file listing
all tests with their associated SVG images.

Usage:
    python generate_svg_html_list.py [--output OUTPUT_FILE]
"""

__all__ = (
    "main",
)

import argparse
import os
from glob import glob

BASE_DIR = os.path.abspath(os.path.dirname(__file__))
DATA_DIR = os.path.join(BASE_DIR, "data")

# XForm suffixes matching the C++ enum in BLI_scanfill_test.cc
XFORM_SUFFIXES = [
    "",  # NONE
    "_rotate_90",
    "_rotate_180",
    "_rotate_270",
    "_rotate_45",
    "_rotate_22_5",
    "_rotate_11_25",
    "_flip_x",
    "_flip_y",
]


def generate_html(output_path: str) -> int:
    """Generate HTML file with side-by-side SVG comparison."""
    json_files = sorted(glob(os.path.join(DATA_DIR, "*.json")))

    if not json_files:
        print("No JSON files found in {:s}".format(DATA_DIR))
        return 1

    html_parts = [
        "<!DOCTYPE html>",
        "<html>",
        "<head>",
        "<meta charset=\"utf-8\">",
        "<title>Scanfill Test SVG Comparison</title>",
        "<style>",
        "body { font-family: sans-serif; margin: 20px; background: #f0f0f0; }",
        "h1 { color: #333; }",
        "h2 { color: #555; margin-top: 30px; border-bottom: 1px solid #ccc; padding-bottom: 5px; }",
        "h3 { color: #666; margin-top: 20px; margin-bottom: 10px; }",
        ".test-group { background: white; padding: 15px; margin: 10px 0; border-radius: 5px; }",
        ".comparison { display: flex; gap: 20px; align-items: flex-start; flex-wrap: wrap; }",
        ".svg-container { text-align: center; }",
        ".svg-container img { max-width: 400px; max-height: 400px; border: 1px solid #ddd; background: white; }",
        ".svg-container p { margin: 5px 0; font-size: 12px; color: #666; }",
        ".missing { color: #999; font-style: italic; padding: 50px; border: 1px dashed #ccc; }",
        ".xform-section { margin-left: 20px; padding: 10px; background: #fafafa; border-radius: 3px; }",
        "</style>",
        "</head>",
        "<body>",
        "<h1>Scanfill Test SVG Comparison</h1>",
        "<p>Side-by-side comparison of standard vs legacy scanfill output.</p>",
    ]

    tests_included = 0
    for json_path in json_files:
        test_name = os.path.splitext(os.path.basename(json_path))[0]

        # Check if any SVGs exist for this test before creating the group
        has_any_svg = False
        for xform_suffix in XFORM_SUFFIXES:
            svg_path = os.path.join(DATA_DIR, test_name + xform_suffix + ".svg")
            svg_legacy_path = os.path.join(DATA_DIR, test_name + xform_suffix + ".legacy.svg")
            if os.path.exists(svg_path) or os.path.exists(svg_legacy_path):
                has_any_svg = True
                break

        if not has_any_svg:
            continue

        tests_included += 1
        html_parts.append("<div class=\"test-group\">")
        html_parts.append("<h2>{:s}</h2>".format(test_name))

        for xform_suffix in XFORM_SUFFIXES:
            xform_label = xform_suffix[1:] if xform_suffix else "none"

            svg_name = test_name + xform_suffix + ".svg"
            svg_legacy_name = test_name + xform_suffix + ".legacy.svg"
            svg_path = os.path.join(DATA_DIR, svg_name)
            svg_legacy_path = os.path.join(DATA_DIR, svg_legacy_name)

            svg_exists = os.path.exists(svg_path)
            svg_legacy_exists = os.path.exists(svg_legacy_path)

            # Only show xform section if at least one SVG exists
            if not svg_exists and not svg_legacy_exists:
                continue

            html_parts.append("<div class=\"xform-section\">")
            html_parts.append("<h3>Transform: {:s}</h3>".format(xform_label))
            html_parts.append("<div class=\"comparison\">")

            # Input polygon SVG
            poly_svg_name = test_name + xform_suffix + ".poly.svg"
            poly_svg_path = os.path.join(DATA_DIR, poly_svg_name)
            html_parts.append("<div class=\"svg-container\">")
            if os.path.exists(poly_svg_path):
                html_parts.append("<a href=\"data/{:s}\">".format(poly_svg_name))
                html_parts.append("<img src=\"data/{:s}\" alt=\"{:s}\">".format(
                    poly_svg_name, poly_svg_name
                ))
                html_parts.append("</a>")
                html_parts.append("<p>Input</p>")
            else:
                html_parts.append("<div class=\"missing\">Input SVG not found</div>")
            html_parts.append("</div>")

            # Standard SVG
            html_parts.append("<div class=\"svg-container\">")
            if svg_exists:
                html_parts.append("<a href=\"data/{:s}\">".format(svg_name))
                html_parts.append("<img src=\"data/{:s}\" alt=\"{:s}\">".format(svg_name, svg_name))
                html_parts.append("</a>")
                html_parts.append("<p>Standard</p>")
            else:
                html_parts.append("<div class=\"missing\">Standard SVG not found</div>")
            html_parts.append("</div>")

            # Legacy SVG
            html_parts.append("<div class=\"svg-container\">")
            if svg_legacy_exists:
                html_parts.append("<a href=\"data/{:s}\">".format(svg_legacy_name))
                html_parts.append("<img src=\"data/{:s}\" alt=\"{:s}\">".format(
                    svg_legacy_name, svg_legacy_name
                ))
                html_parts.append("</a>")
                html_parts.append("<p>Legacy</p>")
            else:
                html_parts.append("<div class=\"missing\">Legacy SVG not found</div>")
            html_parts.append("</div>")

            html_parts.append("</div>")  # comparison
            html_parts.append("</div>")  # xform-section

        html_parts.append("</div>")  # test-group

    html_parts.extend([
        "</body>",
        "</html>",
    ])

    with open(output_path, "w") as fh:
        fh.write("\n".join(html_parts))

    print("Generated: {:s}".format(output_path))
    print("Included {:d} of {:d} test files (with SVGs)".format(tests_included, len(json_files)))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate HTML comparison of legacy vs non-legacy scanfill SVGs."
    )
    parser.add_argument(
        "--output", "-o",
        default=os.path.join(BASE_DIR, "svg_comparison.html"),
        help="Output HTML file path (default: svg_comparison.html in script directory)",
    )
    args = parser.parse_args()

    return generate_html(args.output)


if __name__ == "__main__":
    import sys
    sys.exit(main())
