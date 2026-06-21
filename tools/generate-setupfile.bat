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
    exit /b 1
)

::
:: Check if the executable exists
::
if not exist ".\target\release\setup.exe" (
    echo ERROR: Expected executable .\target\release\setup.exe not found
    echo Make sure your Cargo.toml has the correct binary name
    cd ..
    exit /b 1
)

echo Build completed successfully!

::
:: touch the setup.exe since a "cargo build" could set the resulting .exe filetime to that of "setupwiz\src\main.rs" filetime.
:: Hence use this obscure 'copy' command.
:: Ref: https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-xp/bb490886(v=technet.10)?redirectedfrom=MSDN
::
cd target\release
echo.
echo Touching executable...
copy /B setup.exe +,, > NUL
cd ..\..\..

echo.
echo Copying executable...

::
:: Go back to parent directory and copy the executable
::
copy ".\setupwiz\target\release\setup.exe" "..\setup.exe" > NUL
if errorlevel 1 (
    echo ERROR: Failed to copy executable
    exit /b 1
)

echo.
echo ====================================
echo Success! setup.exe has been copied to ..\setup.exe
echo ====================================

