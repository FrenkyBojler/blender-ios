@echo off
setlocal enabledelayedexpansion

REM ###########################################################################
REM #
REM # This script assumes the machine has been prepared with the vmprep.cmd
REM # script and will build the dependencies in the c:\db folder
REM # This script is also downloaded by vmprep.cmd into the right directory
REM #
REM ###########################################################################

if "%PROCESSOR_ARCHITECTURE%"=="ARM64" (
  set ARCH=arm64
) else if "%PROCESSOR_ARCHITECTURE%"=="AMD64" (
  set ARCH=x64
) else (
  echo Not supported: %PROCESSOR_ARCHITECTURE%
  goto :EOF
)

set CMAKE_GENERATOR_INSTANCE=c:\vs2022bt\
set CMAKE_GENERATOR=Visual Studio 17 2022
set NODEBUG=
set TMPDIR=c:\t\
set PERL=c:\db\build\downloads\perl\perl\bin\perl.exe
set path=%path%;c:\db\build\downloads\perl\perl\bin\

for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format o"') do set START=%%i
call c:\blendergit\blender\build_files\build_environment\windows\build_deps.cmd 2022 %ARCH%
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format o"') do set END=%%i

powershell -NoProfile -Command "$s=[datetime]::Parse('%START%'); $e=[datetime]::Parse('%END%'); Write-Host 'Elapsed:' ($e-$s)"
