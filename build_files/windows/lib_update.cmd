if "%BUILD_ARCH%" == "arm64" (
	set BUILD_VS_LIBDIR=lib/windows_arm64
) else (
	set BUILD_VS_LIBDIR=lib/windows_x64
)

:RETRY
"%GIT%" -C "%BLENDER_DIR%\" config --local "submodule.%BUILD_VS_LIBDIR%.update" "checkout"
"%GIT%" -C "%BLENDER_DIR%\" submodule update --progress --init "%BUILD_VS_LIBDIR%"
if errorlevel 1 (
		set /p LibRetry= "Error during update, retry? y/n"
		if /I "!LibRetry!"=="Y" (
			rem Basic cleanup:
			rem NOTE: the cleanup is only tried during the second retry!
			rmdir /s /q ".git\modules\%BUILD_VS_LIBDIR%"
			"%GIT%" config --remove-section "submodule.%BUILD_VS_LIBDIR%" || echo failed to remove "submodule.%BUILD_VS_LIBDIR%" from git config. Remove it manually please.
			rmdir /s /q "%BUILD_VS_LIBDIR%"
			mkdir "%BUILD_VS_LIBDIR%" 2>nul
			goto RETRY
		)
		echo.
		echo Error: Download of external libraries failed. 
		echo Until this is resolved you CANNOT make a successful blender build.
		echo.
		exit /b 1
)
REM re-detect the dependencies after updating the libraries so any python version 
REM changes are accounted for. 
call "%~dp0\find_dependencies.cmd"
