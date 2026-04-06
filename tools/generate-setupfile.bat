@echo off
setlocal enabledelayedexpansion
cd %~dp0

echo ====================================
echo Rust Project Build Script
echo ====================================

::
:: Check if Rust is installed
::
echo Checking for Rust installation...
rustc --version >nul 2>&1
if errorlevel 1 (
    echo ERROR: Rust is not installed or not in PATH
    echo Please install Rust from https://rustup.rs/
    pause
    exit /b 1
) else (
    echo Rust is installed:
    rustc --version
    cargo --version
)

echo.

::
:: Change to the project directory
::
if not exist ".\setupwiz" (
    echo ERROR: Directory .\setupwiz does not exist
    pause
    exit /b 1
)

echo Building Rust project in release mode...
cd .\setupwiz

::
:: Build the project in release mode
::
cargo build --release
if errorlevel 1 (
    echo ERROR: Build failed
    cd ..
    pause
    exit /b 1
)

echo Build completed successfully!

::
:: Check if the executable exists
::
if not exist ".\target\release\setup.exe" (
    echo ERROR: Expected executable .\target\release\setup.exe not found
    echo Make sure your Cargo.toml has the correct binary name
    cd ..
    pause
    exit /b 1
)

echo.
echo Copying executable...

::
:: Go back to parent directory and copy the executable
::
cd ..
copy ".\setupwiz\target\release\setup.exe" "..\setup.exe"
if errorlevel 1 (
    echo ERROR: Failed to copy executable
    pause
    exit /b 1
)

echo.
echo ====================================
echo Success! setup.exe has been copied to ..\setup.exe
echo ====================================

pause
