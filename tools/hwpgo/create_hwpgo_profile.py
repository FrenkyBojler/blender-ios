import os
import sys
import shutil
import logging
import argparse
import subprocess
from pathlib import Path

from hwpgo_utils import validate_modules, check_llvm_tools

logger = logging.getLogger("create_hwpgo_profiles")
logging.basicConfig(level=logging.INFO)

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--blender", required=True, type=Path, help="Path to the blender.exe that was compiled with hwpgo debug symbols")
    parser.add_argument("--event-collection", required=True, type=Path, help="Path with performance event collections")
    parser.add_argument("--profile", required=True, type=Path, help="Output path for the llvm hwpgo profile")
    parser.add_argument("--cache", required=False, type=Path, default=Path(".cache"), help="A cache for scratch data")
    parser.add_argument("--llvm", required=True, type=Path, help="Path to the LLVM bin dir")

    args = parser.parse_args()

    collections_dir = args.event_collection.resolve()
    blender_exe_path = args.blender.resolve()
    llvm_bin_dir = args.llvm.resolve()
    final_profile = args.profile.resolve()
    cache = args.cache.resolve()

    # Reset cache to avoid stale data from previous runs
    if cache.exists():
        shutil.rmtree(cache)
    cache.mkdir(parents=True, exist_ok=True)

    
    if not blender_exe_path.exists():
        raise FileNotFoundError(f"Blender executable not found: {blender_exe_path}")

    check_llvm_tools(llvm_bin_dir, ["llvm-readobj.exe","llvm-profgen.exe", "llvm-profdata.exe"])

    llvm_readobj_exe = llvm_bin_dir / "llvm-readobj.exe"
    llvm_profgen_exe = llvm_bin_dir / "llvm-profgen.exe"
    llvm_profdata_exe = llvm_bin_dir / "llvm-profdata.exe"

    # Check for which binaries in the blender folder we can create profiles
    blender_dir_path = blender_exe_path.parent
    logger.info(f"Checking binaries in {blender_dir_path} for debug symbols")
    module_symbol_counts = validate_modules(blender_dir_path, llvm_readobj_exe)

    if not module_symbol_counts:
        logger.error(f"Found no debug symbols in binaries in {blender_dir_path}. Cant create llvm profile.")
        sys.exit(1)
    for module in module_symbol_counts.keys():
        logger.info(f"Found {module_symbol_counts[module]} symbols for {module}.")


    # Find collections in collections_dir
    logger.info(f"Looking for perf collections in {collections_dir}")
    collections = list(collections_dir.glob("**/*.perf.data.script"))
    collections_relative = [str(c.relative_to(collections_dir)) for c in collections]
    logger.info("Found %d perf collections: %s.", len(collections_relative), " ".join(collections_relative))   

    # Match collections with binaries that have debug symbols
    commands = []
    for module in module_symbol_counts.keys():
        module_cache = cache / module.with_suffix(".dir").name
        os.makedirs(module_cache, exist_ok=True)
        for collection in collections:
            profile = module_cache / (collection.with_suffix(f".{module.name}.prof").name)
            commands.append([str(llvm_profgen_exe), "--binary", str(module), "--perfscript", str(collection), "--output", str(profile)])
    logger.debug(f"Running profgen commands: {commands}")
    procs = [ subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) for cmd in commands ]
    results = []

    #Wait for all subprocesses to finish and collect stdout stderr and return code
    for p in procs:
        stdout, stderr = p.communicate()
        results.append((p.returncode,stdout,stderr))

    all_succeeded = True
    for i, output in enumerate(results):
        returncode = output[0]
        stdout = output[1]
        stderr = output[2]
        if returncode != 0:
            logger.warning("Profile creation failed for %s %s", commands[i][2], commands[i][4])
            logger.warning("returncode: %s", returncode)
            logger.warning("stdout: %s", stdout)
            logger.warning("stderr: %s", stderr)
            all_succeeded = False
    if all_succeeded:
        logger.info("Successfully created profiles for all module - collection combinations")


    #Merge all profiles
    profiles = list(cache.glob("**/*.prof"))

    merge_cmd = [str(llvm_profdata_exe), "merge", "--text", "--sample", "--output", str(final_profile)] + [str(p) for p in profiles]
    logger.debug(f"Merging profdata: {merge_cmd}")
    merge_result = subprocess.run(merge_cmd, capture_output=True, text=True, check=False)

    if merge_result.returncode != 0:
        logger.warning("Profile merge failed.")
        logger.warning("returncode: %s", merge_result.returncode)
        logger.warning("stdout: %s", merge_result.stdout)
        logger.warning("stderr: %s", merge_result.stderr)
        sys.exit(1)
    else:
        logger.info(f"Wrote merged profile to {final_profile}")

    # Clean up cache, the merged profile is the only output we need
    try:
        shutil.rmtree(cache)
    except OSError as e:
        logger.warning("Could not remove cache directory %s: %s", cache, e)

if __name__ == "__main__":
    raise SystemExit(main())
