:: C:\detached_build.cmd
set VSBT_YEAR=2022

if "%PROCESSOR_ARCHITECTURE%"=="ARM64" (
  set C:\vs%VSBT_YEAR%bt\VC\Auxiliary\Build\vcvarsarm64.bat
) else if "%PROCESSOR_ARCHITECTURE%"=="AMD64" (
  set VCVARS_PATH=C:\vs%VSBT_YEAR%bt\VC\Auxiliary\Build\vcvars64.bat
) else (
  echo Not supported: %PROCESSOR_ARCHITECTURE%
  goto :EOF
)

@echo off
call "%VCVARS_PATH%"
cd /d C:\db
call build.cmd > build.log 2>&1
