@echo off
REM This is a helper script to easily force a rebuild of a single dependency
REM calling nuke depname in c:\db will remove all build artifacts of the 
REM dependency and the next time you call vmbuild the dep will be build from 
REM scratch. 
if "%1"=="" goto EOF:
set ROOT=%~dp0\build\

REM walk every subfolder under "s" (vs1764D, vs1564R, x64, debug, build, arm64, future names)
REM %%D loop var, checks two names each folder: %1 and external_%1
for /d /r "%ROOT%\s" %%D in (%1 external_%1) do (
  REM nul trick = confirm folder real, not junk match
  if exist "%%D\nul" echo removing "%%D" && rd /s /q "%%D"
)

REM walk every subfolder under "output" (win64_vc15, winarm64_vc15, future ones)
for /d /r "%ROOT%\output" %%D in (%1) do (
  if exist "%%D\nul" echo removing "%%D" && rd /s /q "%%D"
)

:EOF