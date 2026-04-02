import sys
import json
import shutil
import logging
import argparse
import subprocess

from json import JSONDecodeError
from pathlib import Path
from hwpgo_utils import validate_modules, check_llvm_tools

logger = logging.getLogger("collect_hwpgo_sep")
logging.basicConfig(level=logging.INFO)

def build_sep_command(sav: int, tb7_path: Path, blender_command: list):
    cmd = ["sep", "-start",
    "-out", str(tb7_path.resolve()),
    "-ec", f'"BR_INST_RETIRED.NEAR_TAKEN:SA={sav}:pdir:lbr:USR=YES"',
    "-lbr", "no_filter:usr", "-perf-script", "ip,brstack",
    "-app", " ".join(blender_command)]
    return cmd

def run_command(cmd, stdout_path, stderr_path):
    logger.info("Running sep command: "+" ".join(cmd))

    with open(stdout_path, "wb") as stdout_file, open(stderr_path, "wb") as stderr_file: 
        proc = subprocess.run(
            cmd,
            stdout=stdout_file,
            stderr=stderr_file,
            check=False,  # we handle rc ourselves
        )
    if proc.returncode != 0:
        logger.warning(
            "Command failed (rc=%s): %s\nstdout -> %s\nstderr -> %s\n",
            proc.returncode, cmd, stdout_path, stderr_path)


def load_workload_json(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        logger.error(f"Could not find workload configuration {path}")
        sys.exit(1)
    except JSONDecodeError as e:
        logger.error(f"Invalid JSON in {path}: {e}")
        sys.exit(1)

def main() -> int:
    parser = argparse.ArgumentParser(description="A tool that performs performance event collections for blender, as required for HWPGO optimization based on a workload json definition.")
    parser.add_argument("--blender", required=True, type=Path, help="Path to the blender.exe that was compiled with hwpgo debug symbols")
    parser.add_argument("--workload", required=True, type=Path, help="Path to a workload description .json")
    parser.add_argument("--event-collection", required=True, type=Path, help="Output path for the performance event collections")
    parser.add_argument("--llvm", required=True, type=Path, help="Path to the LLVM bin dir")

    args = parser.parse_args()
    workload_path = args.workload.resolve()
    blender_exe_path = args.blender.resolve()
    llvm_bin_dir = args.llvm.resolve()

    sep_cmd = shutil.which("sep")
    if sep_cmd is None:
        raise FileNotFoundError("Could not find 'sep' on PATH. "
            "Make sure it is installed and the environment is activated.")

    logger.info(f"Found sep at: {sep_cmd}")

    if not workload_path.exists():
        raise FileNotFoundError(f"Workload path not found: {workload_path}")
    if not blender_exe_path.exists():
        raise FileNotFoundError(f"Blender executable not found: {blender_exe_path}")

    check_llvm_tools(llvm_bin_dir, ["llvm-readobj.exe"])
    llvm_readobj_exe = llvm_bin_dir / "llvm-readobj.exe"

    workload_dict = load_workload_json(workload_path)
    out_dir = args.event_collection

    # Check binaries in blender folder for required symbols
    blender_dir_path = blender_exe_path.parent
    logger.info(f"Checking binaries in {blender_dir_path} for debug symbols")
    module_symbol_counts = validate_modules(blender_dir_path, llvm_readobj_exe)

    if not module_symbol_counts:
        logger.error(f"Found no debug symbols in binaries in {blender_dir_path}.")
        sys.exit(1)
    for module in module_symbol_counts.keys():
        logger.info(f"Found {module_symbol_counts[module]} symbols in {module} debug info")

    # Run all workloads
    workload_name = workload_dict["name"]
    workload_dir = out_dir / workload_name
    workload_dir.mkdir(parents=True, exist_ok=True)
    for scene in workload_dict["workloads"]:
        name = scene["name"]
        scene_args = scene["args"]
        output_tb7 = workload_dir / f"collection_{name}.tb7"
        cmd = build_sep_command(4000003, output_tb7.resolve(), [str(blender_exe_path)] + scene_args)
        stdout_path = workload_dir / f"stdout_{name}.txt"
        stderr_path = workload_dir / f"stderr_{name}.txt"
        run_command(cmd, stdout_path, stderr_path)

if __name__ == "__main__":
    raise SystemExit(main())




