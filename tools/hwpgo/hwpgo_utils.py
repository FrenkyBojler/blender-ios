import logging
import subprocess
from pathlib import Path

logger = logging.getLogger(__name__)

def validate_modules(blender_dir_path, llvm_readobj_exe):
    modules = find_binaries(blender_dir_path)
    
    module_symbol_counts = {}
    for module in modules:
        cmd = [str(llvm_readobj_exe)]
        cmd.append("--symbols")
        cmd.append(str(module.resolve()))
        result = subprocess.run(cmd, capture_output=True, encoding="utf-8", errors="replace", check=False)
        if result.returncode != 0:
            logger.debug("llvm-readobj failed for %s: %s", module, result.stderr)
            continue
        nsymbols = result.stdout.count("Symbol")
        if nsymbols > 1: #Default symbol is always present
            module_symbol_counts[module.resolve()] = nsymbols

    return module_symbol_counts

def find_binaries(root: Path):
    return [
        p for p in root.rglob("*")
        if p.suffix.lower() in {".exe", ".dll"}
    ]

def check_llvm_tools(llvm_bin_path, tools):
    if not llvm_bin_path.exists():
        raise FileNotFoundError(f"Could not find llvm bin dir at: {llvm_bin_path}")

    for tool in tools:
        llvm_tool_exe = llvm_bin_path / tool
        if not llvm_tool_exe.exists():
            raise FileNotFoundError(f"Could not find {tool}: {llvm_tool_exe}")

