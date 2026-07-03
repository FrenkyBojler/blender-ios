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
  set CUDA_PATH =C:\tools\cuda\12.8.0
  set CUDA_PATH_V12_8=C:\tools\cuda\12.8.0
  set ROCM_PATH=c:\tools\rocm\7.1.1
  set HIP_PATH=c:\tools\rocm\7.1.1
  set HIP_PATH_71=c:\tools\rocm\7.1.1
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

if not exist c:\db\ mkdir c:\db
cd /d c:\db

set START=!TIME!
call c:\blendergit\blender\build_files\build_environment\windows\build_deps.cmd 2022 %ARCH%
set END=!TIME!

echo Start: !START!
echo End:   !END!
powershell -NoProfile -Command "$s=[datetime]::ParseExact('%START%','H:mm:ss.ff',$null); $e=[datetime]::ParseExact('%END%','H:mm:ss.ff',$null); if($e -lt $s){$e=$e.AddDays(1)}; Write-Host 'Elapsed:' ($e-$s)"
