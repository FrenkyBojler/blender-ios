# SPDX-FileCopyrightText: 2019-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Utility script, called by ``blender_headless.py`` to run inside Blender to avoid boilerplate code having to be added
into each test. Handles execution primarily for CTest usage.

See ``run.py`` for an alternate helper script with more utilities helpful when debugging or developing these tests.
"""

import os
import sys


def create_parser():
    import argparse
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawTextHelpFormatter,
    )

    parser.add_argument(
        "--tests",
        dest="tests",
        nargs='+',
        required=True,
        metavar="TEST_ID",
        help="Names of tests to run.",
    )

    return parser


def main():
    directory = os.path.dirname(__file__)
    sys.path.insert(0, directory)
    if "bpy" not in sys.modules:
        raise Exception("This must run inside Blender")
    import bpy
    import gpu

    parser = create_parser()
    args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:])
    verbose = os.getenv('BLENDER_VERBOSE') is not None

    def on_error():
        sys.exit(1)

    def on_exit():
        sys.exit(0)

    gpu_device = gpu.platform.device_type_get()

    BLOCKLIST = []
    if os.getenv("BLENDER_TEST_IGNORE_BLOCKLIST") is None:
        if sys.platform == "win32" and gpu_device == "INTEL":
            # See #149084 for the tracking issue
            BLOCKLIST = ["test_workspace"]

    is_first = True
    for test_id in args.tests:
        mod_name, fn_name = test_id.partition(".")[0::2]

        if mod_name in BLOCKLIST or test_id in BLOCKLIST:
            if not args.keep_open:
                sys.exit(0)

        if not is_first:
            bpy.ops.wm.read_homefile()
        is_first = False

        mod = __import__(mod_name)
        test_fn = getattr(mod, fn_name)

        from modules import easy_keys

        # So we can get the operator ID's.
        bpy.context.preferences.view.show_developer_ui = True

        # Hack back in operator search.

        easy_keys.setup_default_preferences(bpy.context.preferences)
        easy_keys.run(
            test_fn(),
            on_error=on_error,
            on_exit=on_exit,
        )


if __name__ == "__main__":
    main()
