import bpy
import os
import sys
import subprocess
import argparse
from dataclasses import dataclass

from pathlib import Path

blend_diff_bin = Path(bpy.app.binary_path).parent / "blend_diff"


def main():
    if "--" in sys.argv:
        raw_args = sys.argv[sys.argv.index("--") + 1:]
    else:
        raw_args = sys.argv[1:]
    parser = argparse.ArgumentParser()
    parser.add_argument("--testdir", required=True, type=Path)
    args = parser.parse_args(raw_args)
    run(args)


def run(args):
    test_dir = Path(args.testdir)
    blends_dir = test_dir / "blends"
    diffs_dir = test_dir / "diffs"
    diffs_dir.mkdir(parents=True, exist_ok=True)

    update_tests = os.getenv("BLENDER_TEST_UPDATE") is not None

    failed_tests = []

    for case in test_cases:
        blend_old_path = blends_dir / case.old_name
        blend_new_path = blends_dir / case.new_name
        diff_name = case.name + ".diff"
        diff_path = diffs_dir / diff_name

        if diff_path.exists():
            expected_diff = diff_path.read_text()
        else:
            failed_tests.append(case)
            print(f"Missing expected diff for {case.name}")
            if not update_tests:
                continue
            expected_diff = None

        diff_args = [str(blend_diff_bin), str(blend_old_path), str(blend_new_path)]
        print(f"Computing diff for {case.name}")
        print(" ".join(diff_args))
        output = subprocess.run(diff_args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if output.returncode != 0:
            failed_tests.append(case)
            print(f"Diff failed for {case.name}")
            print(output.stderr.decode("utf-8"))
            continue
        actual_diff = output.stdout.decode("utf-8")
        if expected_diff is not None:
            if actual_diff == expected_diff:
                print(f"Diff is correct")
                continue

        if update_tests:
            print(f"Test is outdated, updating {case.name}")
            Path(diff_path).write_text(actual_diff)

    if len(failed_tests) > 0:
        print(f"{len(failed_tests)} / {len(test_cases)} tests failed")
        print("Failing tests have been updated")
        sys.exit(1)
    print("All tests ok.")


@dataclass
class TestCase:
    old_name: str
    new_name: str

    @property
    def name(self):
        return self.old_name.removesuffix(".blend") + "_VS_" + self.new_name.removesuffix(".blend")


test_cases = (
    TestCase("start_5_0.blend", "start_5_0_moved_camera.blend"),
    TestCase("start_5_0.blend", "start_5_0_moved_vertex.blend"),
)


if __name__ == "__main__":
    main()
