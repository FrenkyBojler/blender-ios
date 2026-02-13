# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# ./blender.bin -b -X --python tests/python/bl_pyapi_doc_gen.py -- --verbose

__all__ = (
    "main",
)

import argparse
import contextlib
import glob
import os
import subprocess
import sys
import tempfile
import unittest


# This file lives at: `./tests/python/bl_pyapi_doc_gen.py`.
SOURCE_ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
SPHINX_DOC_GEN_DIR = os.path.join(SOURCE_ROOT, "doc", "python_api")

PYTHON_BIN = os.environ.get("PYTHON_BIN") or sys.executable


class Global:
    # When set (via `--output`) the test writes its output here instead of a
    # temporary directory, and the contents are NOT removed afterwards. Useful
    # for troubleshooting invalid output.
    user_output_dir: str | None = None


@contextlib.contextmanager
def sys_argv_and_path_override(argv, path_prepend):
    """Temporarily replace ``sys.argv`` and prepend to ``sys.path``."""
    argv_bak = sys.argv
    path_bak = sys.path[:]
    sys.argv = argv
    sys.path.insert(0, path_prepend)
    try:
        yield
    finally:
        sys.argv = argv_bak
        sys.path[:] = path_bak


@contextlib.contextmanager
def output_dir_context():
    """
    Yield the output directory.

    When ``Global.user_output_dir`` is set (via ``--output``), use that path and
    leave its contents in place after the test ends. Otherwise create a
    ``tempfile.TemporaryDirectory`` that is removed on exit.
    """
    if Global.user_output_dir is None:
        with tempfile.TemporaryDirectory() as tmpdir:
            yield tmpdir
    else:
        os.makedirs(Global.user_output_dir, exist_ok=True)
        yield Global.user_output_dir


class DocGenTest(unittest.TestCase):

    def test_doc_gen_and_stubs(self) -> None:
        with output_dir_context() as output_dir:
            rst_dir = os.path.join(output_dir, "sphinx-in")
            stubs_dir = os.path.join(output_dir, "stubs")

            doc_gen_ok = False
            with self.subTest("sphinx_doc_gen"):
                # `sphinx_doc_gen` parses `sys.argv` at import time (via its
                # module-level `ARGS = handle_args()`), so override `sys.argv`
                # and `sys.path` for the duration of the import. Drop any cached
                # module so `ARGS` is re-evaluated from the argv set here.
                argv = [
                    os.path.join(SPHINX_DOC_GEN_DIR, "sphinx_doc_gen.py"),
                    "--",
                    "--output", output_dir,
                ]
                sys.modules.pop("sphinx_doc_gen", None)
                with sys_argv_and_path_override(argv, SPHINX_DOC_GEN_DIR):
                    import sphinx_doc_gen  # type: ignore[import-not-found]
                    rc = sphinx_doc_gen.main()

                self.assertEqual(0, rc, "sphinx_doc_gen.main() returned non-zero")
                self.assertTrue(os.path.isdir(rst_dir), "missing directory: {:s}".format(rst_dir))
                rst_files = glob.glob(os.path.join(rst_dir, "*.rst"))
                self.assertTrue(rst_files, "no RST files generated in {:s}".format(rst_dir))
                doc_gen_ok = True

            if not doc_gen_ok:
                return

            with self.subTest("sphinx_stub_gen"):
                stub_gen_script = os.path.join(SPHINX_DOC_GEN_DIR, "sphinx_stub_gen.py")
                result = subprocess.run(
                    [PYTHON_BIN, stub_gen_script, rst_dir, "-o", stubs_dir],
                    check=False,
                )
                self.assertEqual(
                    0, result.returncode,
                    "sphinx_stub_gen.py failed (returncode={:d})".format(result.returncode),
                )
                self.assertTrue(os.path.isdir(stubs_dir), "missing directory: {:s}".format(stubs_dir))
                pyi_files = glob.glob(os.path.join(stubs_dir, "**", "*.pyi"), recursive=True)
                self.assertTrue(pyi_files, "no .pyi files generated in {:s}".format(stubs_dir))


def main() -> None:
    extra_argv = []
    if "--" in sys.argv:
        extra_argv = sys.argv[sys.argv.index("--") + 1:]

    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument(
        "--output",
        default=None,
        help=(
            "Output directory for generated files. "
            "When set, the directory is created if missing and its contents are NOT removed "
            "after the test runs - useful for troubleshooting invalid output."
        ),
    )
    args, unittest_argv = parser.parse_known_args(extra_argv)
    Global.user_output_dir = args.output

    unittest.main(argv=[sys.argv[0]] + unittest_argv)


if __name__ == "__main__":
    main()
