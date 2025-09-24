@echo off
setlocal enabledelayedexpansion

::
:: Find setup.py in current directory or tools directory
::
set SETUP_PATH=
if exist "setup.py" (
    set "SETUP_PATH=setup.py"
) else if exist "tools\setup.py" (
    set "SETUP_PATH=tools\setup.py"
)

if not defined SETUP_PATH (
    echo ERROR: setup.py not found in current directory or tools directory
    pause
    exit /b 1
)

echo Found setup.py at: !SETUP_PATH!

echo Checking for PyInstaller...

::
:: Check if pyinstaller.exe is in PATH
::
where pyinstaller.exe >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: pyinstaller.exe not found in PATH
    echo Please install PyInstaller or add it to your PATH
    pause
    exit /b 1
)

echo PyInstaller found in PATH

echo Building executable with PyInstaller...
pyinstaller --onefile !SETUP_PATH!

::
:: Check if PyInstaller succeeded
::
if %errorlevel% neq 0 (
    echo ERROR: PyInstaller failed to build executable
    pause
    exit /b 1
)

::
:: Check if the executable was created
::
if not exist "dist\setup.exe" (
    echo ERROR: dist\setup.exe was not created
    pause
    exit /b 1
)

::
:: Move executable to appropriate location
::
set "CURDIR=%CD%"
for %%I in ("%CURDIR%") do set "CURFOLDER=%%~nxI"
if /I "!CURFOLDER!"=="tools" (
    echo Moving executable to parent directory...
    move /Y "dist\setup.exe" "..\setup.exe"
) else (
    echo Moving executable to current directory...
    move /Y "dist\setup.exe" "setup.exe"
)

if %errorlevel% neq 0 (
    echo ERROR: Failed to move setup.exe
    pause
    exit /b 1
)

echo Cleaning up generated files...

::
:: Remove setup.spec file
::
if exist "setup.spec" (
    del "setup.spec"
    echo Removed setup.spec
)

::
:: Remove build folder
::
if exist "build" (
    rmdir /s /q "build"
    echo Removed build folder
)

::
:: Remove dist folder
::
if exist "dist" (
    rmdir /s /q "dist"
    echo Removed dist folder
)

echo.
echo Build completed successfully!
echo Executable created: setup.exe
::
:: Sleep for 3 sec
::
ping.exe -4 -n 3 localhost >nul

