@echo off
rem ---------------------------------------------------------------------------
rem Blender build environment wrapper (local dev scaffolding, not for the PR).
rem
rem Activates the MSVC developer environment (vcvars64) plus the clang-cl,
rem cmake, ninja, and sccache directories that the CMakePresets.json presets
rem expect, then runs whatever command is passed as arguments from the source
rem tree at C:\dev\blender\main.
rem
rem Usage:
rem   bl_env.bat cmake --preset relwithdebinfo          (configure)
rem   bl_env.bat cmake --build --preset relwithdebinfo  (build; default type)
rem
rem Build output lands in C:\dev\blender\build_windows_x64_clang_RelWithDebInfo
rem (presets place build dirs beside the source, not inside it).
rem ---------------------------------------------------------------------------
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin;C:\dev\sculptcore\emsdk\cmake\4.2.0-rc3_64bit\bin;C:\dev\sculptcore\emsdk\ninja\git-release_64bit\bin;C:\dev\sccache\target\release;%PATH%"
cd /d C:\dev\blender\main
%*
