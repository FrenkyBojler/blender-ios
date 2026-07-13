@echo off
REM This is a helper script to easily force a rebuild of a single dependency
REM calling nuke depname in c:\db will remove all build artifacts of the 
REM dependency and the next time you call build the dep will be build from 
REM scratch. 
@echo off
REM depname required, else bail
if "%~1"=="" goto :EOF
REM %~dp0 already end backslash, no extra dash needed
set "ROOT=%~dp0build"

REM two folder name patterns: plain and external_ prefix
for %%N in (%1 external_%1) do (
  REM dir /s = full list first, no live-mutate walk like old for /r
  for /f "delims=" %%D in ('dir /ad /b /s "%ROOT%\s\%%N" 2^>nul') do (
    REM parent folder may already delete child match, skip if gone
    if exist "%%D" (
      echo removing "%%D"
      rd /s /q "%%D"
    )
  )
)
REM output tree only plain name, no external_ variant there
for /f "delims=" %%D in ('dir /ad /b /s "%ROOT%\output\%1" 2^>nul') do (
  if exist "%%D" (
    echo removing "%%D"
    rd /s /q "%%D"
  )
)