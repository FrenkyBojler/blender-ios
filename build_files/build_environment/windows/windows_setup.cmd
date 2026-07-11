@echo off
setlocal enabledelayedexpansion

REM Visual Studio Build Tools 17.14.14
REM https://learn.microsoft.com/en-us/visualstudio/releases/2022/release-history
set VSBT_VER=17.14.14
set VSBT_YEAR=2022
set VSBT_URL=https://download.visualstudio.microsoft.com/download/pr/c2e2845d-bdff-44fc-ac00-3d488e9f5675/abdb87ff12fe1885ccc4964b4ab51da566549fd2096d7ceb1fdea907372f21d7/vs_BuildTools.exe

set GIT_VER_X64=2.38.0
set GIT_VER_ARM64=2.54.0

set CMAKE_VER_X64=3.31.7
set CMAKE_VER_ARM64=3.31.12

set MESON_VER_X64=1.9.1
set MESON_VER_ARM64=1.11.0

set LLVM_VER_ARM64=20.1.8

set CUDA_VER=12.8
set CUDA_FULL_VER=12.8.0
set CUDA_BUILD=571.96
set HIP_VER=7.1
set HIP_FULL=7.1.51803
set VCREDIST_URL=https://download.microsoft.com/download/3/2/2/3224B87F-CFA0-4E70-BDA3-3DE650EFEBA5/vcredist_x64.exe
set HIP_URL=https://download.amd.com/developer/eula/rocm-hub/AMD-Software-PRO-Edition-26.Q1-Win11-For-HIP.exe
set SEVENZIP_URL=https://github.com/ip7z/7zip/releases/download/26.02/7zr.exe

set UNATTENDED=0
set BRANCH=main
set REPO=blender/blender

REM Usage examples: windows_setup.cmd 
REM                 windows_setup.cmd unattended repo myuser/blender my-branch
REM                 windows_setup.cmd repo blender/blender blender-v5.2-release
:parse_args
if "%1"=="unattended" (set UNATTENDED=1& shift & goto :parse_args)
if "%1"=="repo" (set REPO=%2& shift & shift & goto :parse_args)
if not "%1"=="" (set BRANCH=%1& shift & goto :parse_args)

if "%PROCESSOR_ARCHITECTURE%"=="ARM64" (
  set ARCH=arm64
) else if "%PROCESSOR_ARCHITECTURE%"=="AMD64" (
  set ARCH=x64
) else (
  echo CPU architecture not supported: %PROCESSOR_ARCHITECTURE%
  goto :EOF
)

if %UNATTENDED%==1 goto skip_confirm
echo **********************************************************************
echo ** Sets up system to build Blender library dependencies.
echo ** Should to be run as Administrator in command prompt on a fresh
echo ** Windows install for ARM64 or x64.
echo **
echo ** The following software will be installed
echo ** - Visual Studio Build Tools
echo ** - Git, CMake, Meson
echo ** - LLVM (ARM64 only)
echo ** - CUDA, ROCm (x64 only)
echo **
echo ** The following directories will be created
echo ** - C:\install\           - Installer files used by this script
echo ** - C:\t\                 - Temp directory used in building Python
echo ** - C:\blendergit\blender - This is the blender source repository
echo ** - C:\db                 - This is the build directory
echo **
echo ** The following scripts will be downloaded into C:\db
echo ** - build.cmd             - Script to initialize build
echo ** - nuke.cmd              - Nuke scripts for rebuilding libraries
echo **
set /p CONFIRM=** Enter DANGER to continue: 
echo **********************************************************************
if not "%CONFIRM%"=="DANGER" (echo Aborted. & goto :EOF)
:skip_confirm

mkdir C:\install
mkdir C:\t

echo Obtaining Visual Studio Build Tools %VSBT_VER% installer
curl -s %VSBT_URL% -o C:\install\vs_BuildTools.exe
echo Obtaining Visual Studio Build Tools %VSBT_VER% config
curl -s https://projects.blender.org/%REPO%/raw/branch/%BRANCH%/build_files/build_environment/windows/.vsconfig_%ARCH% -o C:\install\.vsconfig
echo Installing Visual Studio Build Tools %VSBT_VER%
C:\install\vs_BuildTools.exe --wait --quiet --norestart --installPath C:\vs%VSBT_YEAR%bt\ --config C:\install\.vsconfig

if "%ARCH%"=="arm64" goto arm_deps
goto x64_deps

:arm_deps
echo Obtaining Git %GIT_VER_ARM64%
curl -s -L https://github.com/git-for-windows/git/releases/download/v%GIT_VER_ARM64%.windows.1/Git-%GIT_VER_ARM64%-arm64.exe -o C:\install\git.exe
echo Installing Git %GIT_VER_ARM64%
start /wait C:\install\git.exe /verysilent /norestart
set PATH=%PATH%;C:\Program Files\Git\cmd

echo Obtaining CMake %CMAKE_VER_ARM64%
curl -s -L https://github.com/Kitware/CMake/releases/download/v%CMAKE_VER_ARM64%/cmake-%CMAKE_VER_ARM64%-windows-arm64.msi -o C:\install\cmake.msi
echo Installing CMake %CMAKE_VER_ARM64%
start /wait msiexec /quiet /norestart /i C:\install\cmake.msi ADD_CMAKE_TO_PATH="System"
set PATH=%PATH%;C:\Program Files\CMake\bin

echo Obtaining Meson %MESON_VER_ARM64%
curl -s -L https://github.com/mesonbuild/meson/releases/download/%MESON_VER_ARM64%/meson-%MESON_VER_ARM64%-64.msi -o C:\install\meson.msi
echo Installing Meson %MESON_VER_ARM64%
start /wait msiexec /quiet /norestart /i C:\install\meson.msi
set PATH=%PATH%;C:\Program Files\Meson

echo Obtaining LLVM %LLVM_VER_ARM64%
curl -s -L https://github.com/llvm/llvm-project/releases/download/llvmorg-%LLVM_VER_ARM64%/LLVM-%LLVM_VER_ARM64%-woa64.exe -o C:\install\llvm.exe
echo Installing LLVM %LLVM_VER_ARM64%
start /wait C:\install\llvm.exe /S
set PATH=%PATH%;C:\Program Files\LLVM\bin

set VCVARS_PATH=C:\vs%VSBT_YEAR%bt\VC\Auxiliary\Build\vcvarsarm64.bat
goto common

:x64_deps
echo Obtaining Git %GIT_VER_X64%
curl -s -L https://github.com/git-for-windows/git/releases/download/v%GIT_VER_X64%.windows.1/Git-%GIT_VER_X64%-64-bit.exe -o C:\install\git.exe
echo Installing Git %GIT_VER_X64%
start /wait C:\install\git.exe /verysilent /norestart
set PATH=%PATH%;C:\Program Files\Git\cmd

echo Obtaining CMake %CMAKE_VER_X64%
curl -s -L https://github.com/Kitware/CMake/releases/download/v%CMAKE_VER_X64%/cmake-%CMAKE_VER_X64%-windows-x86_64.msi -o C:\install\cmake.msi
echo Installing CMake %CMAKE_VER_X64%
start /wait msiexec /quiet /norestart /i C:\install\cmake.msi ADD_CMAKE_TO_PATH="System"
set PATH=%PATH%;C:\Program Files\CMake\bin

echo Obtaining Meson %MESON_VER_X64%
curl -s -L https://github.com/mesonbuild/meson/releases/download/%MESON_VER_X64%/meson-%MESON_VER_X64%-64.msi -o C:\install\meson.msi
echo Installing Meson %MESON_VER_X64%
start /wait msiexec /quiet /norestart /i C:\install\meson.msi
set PATH=%PATH%;C:\Program Files\Meson\

echo Obtaining VS2010 redist
curl -s %VCREDIST_URL% -o C:\install\vcredist_x64.exe
echo Installing VS2010 redist
start /wait C:\install\vcredist_x64.exe /passive /norestart

echo Obtaining CUDA %CUDA_VER%
curl -s https://developer.download.nvidia.com/compute/cuda/%CUDA_FULL_VER%/local_installers/cuda_%CUDA_FULL_VER%_%CUDA_BUILD%_windows.exe -o C:\install\cuda.exe
echo Installing CUDA %CUDA_VER%
start /wait C:\install\cuda.exe -s -n nvcc_%CUDA_VER% cudart_%CUDA_VER% nvrtc_%CUDA_VER% nvrtc_dev_%CUDA_VER% nvjitlink_%CUDA_VER% nvtx_%CUDA_VER% thrust_%CUDA_VER% curand_%CUDA_VER% curand_dev_%CUDA_VER%
echo Moving CUDA %CUDA_VER%
robocopy "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v%CUDA_VER%" "C:\tools\cuda\%CUDA_VER%" /E /MOVE /NJH /NJS /NP /NFL /NDL
setx CUDA_PATH "C:\tools\cuda\%CUDA_VER%" /M > nul
setx CUDA_PATH_V12_8 "C:\tools\cuda\%CUDA_VER%" /M > nul

echo Obtaining 7-Zip
curl -s -L %SEVENZIP_URL% -o C:\install\7zr.exe
set PATH=%PATH%;C:\install

echo Obtaining HIP SDK %HIP_FULL%
curl -s %HIP_URL% -o C:\install\hip.exe
echo Extracting HIP SDK %HIP_FULL%
7zr.exe x C:\install\hip.exe -oC:\install\ -y > nul
mkdir C:\tools\rocm
echo Installing ROCm from HIP SDK %HIP_FULL%
start /wait msiexec /quiet /norestart /i C:\install\Packages\Apps\ROCmSDKPackages\SDKCore\ROCm_SDK_Core.msi INSTALLDIR="C:\tools\rocm"
setx HIP_PATH "C:\tools\rocm\%HIP_VER%" /M > nul
setx HIP_PATH_71 "C:\tools\rocm\%HIP_VER%" /M > nul

set VCVARS_PATH=C:\vs%VSBT_YEAR%bt\VC\Auxiliary\Build\vcvars64.bat
goto common

:common
REM NuGet may be initialized without sources (see https://github.com/python/cpython/pull/152919)
echo Obtaining NuGet CLI
curl -s -L https://dist.nuget.org/win-x86-commandline/v7.6.0/nuget.exe -o C:\install\nuget.exe
echo Initializing NuGet with source
start /wait C:\install\nuget.exe sources add -Name nuget.org -Source https://api.nuget.org/v3/index.json >nul 2>nul

mkdir C:\blendergit
cd C:\blendergit
if exist blender (
  echo Repo dir exists, removing..
  rd /q /s blender
)
echo Cloning Blender repository
git clone --quiet --branch %BRANCH% https://projects.blender.org/%REPO%.git

mkdir C:\db
echo Copying build and nuke scripts
copy /y C:\blendergit\blender\build_files\build_environment\windows\*.cmd C:\db\ > nul
echo Done

echo **********************************************************************
echo ** Run "call %VCVARS_PATH%" or
echo ** Open "%ARCH% Native Tools Command Prompt for VS %VSBT_YEAR%"
echo ** Then run "cd C:\db && build.cmd" to start a build.
echo **********************************************************************
