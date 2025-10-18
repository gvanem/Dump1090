@echo off
setlocal EnableDelayedExpansion
prompt $P$G

::
:: Download-links for clang/x64:
::
set URL_CLANG_EXE=https://github.com/llvm/llvm-project/releases/download/llvmorg-20.1.7/LLVM-20.1.7-win64.exe

::
:: 'APPVEYOR_PROJECT_NAME=Dump1090' unless testing this as:
::   c:\dev\Dump1090\.github> cmd /c appveyor-build.bat
::
:: locally.
::
:: Change value of '_ECHO' to an 'echo.exe' with colour support.
:: Like Cygwin or MSys.
::
if %APPVEYOR_PROJECT_NAME%. == . (
  set LOCAL_TEST=1
  set _ECHO=%CYGWIN_ROOT%\bin\echo.exe -e
  set APPVEYOR_BUILD_FOLDER=%CD%\..

) else (
  set LOCAL_TEST=0
  set APPVEYOR_BUILD_FOLDER=c:\projects\Dump1090
  set _ECHO=c:\msys64\usr\bin\echo.exe -e
  set PYTHON=py -3
)

::
:: Download stuff to here:
::
set CI_ROOT=%APPVEYOR_BUILD_FOLDER%\CI-temp
md %CI_ROOT% 2> NUL

::
:: For '..\dump1090 -VV'
::
set COLUMNS=100

::
:: Sanity check:
::
if %BUILDER%. == . (
  %_ECHO% "\e[1;31mBUILDER target not specified!\e[0m"
  exit /b 1
)

cd ..\src

::
:: Local 'cmd' test for '(' in env-vars:
:: This is what AppVeyor have first in their PATH:
::   c:\Program Files (x86)\Microsoft SDKs\Azure\CLI2\wbin
::
:: Hence we cannot use a 'if x (what-ever) else (something else)' syntax with that.
::
if %LOCAL_TEST% == 1 (
  if not exist "%APPVEYOR_BUILD_FOLDER%" (echo No '%APPVEYOR_BUILD_FOLDER%'. Edit this .bat-file & exit /b 1)
)

%_ECHO% "\e[1;33m--------------------------------------------------------------------------------------------------\e[0m"

if %BUILDER%. == MSVC. (
  %_ECHO% "\e[1;33mBuilding for MSVC/x64:\e[0m"
  make -f Makefile.Windows CC=cl CPU=x64 USE_PACKED_DLL=1 USE_BIN_FILES=1 USE_MP_COMPILE=1 clean all
  goto run_tests
)

::
:: Need to do 'call :install_clang' here to set the PATH for 'clang-cl.exe'!
::
if %BUILDER%. == clang. (
  call :install_clang
  %_ECHO% "\e[1;33mBuilding for clang-cl/x64:\e[0m"
  make -f Makefile.Windows CC=clang-cl CPU=x64 USE_PACKED_DLL=1 USE_BIN_FILES=1 clean all
  goto run_tests
)

if %BUILDER%. == MinGW. (
  %_ECHO% "\e[1;33mgcc info:\e[0m"
  gcc -v
  %_ECHO% "\e[1;33mBuilding for MinGW/x64:\e[0m"
  make -f Makefile.MinGW CPU=x64 USE_PACKED_DLL=1 USE_BIN_FILES=1 clean all
  goto run_tests
)

%_ECHO% "\e[1;31mIllegal BUILDER (BUILDER=%BUILDER%) values! Remember cmd.exe is case-sensitive.\e[0m"
exit /b 1

::
:: Download the '%CI_ROOT%\LLVM-*win64.exe' for 64-bit 'clang-cl'.
:: A 300 MByte download which silently installs to "c:\Program Files\LLVM"
::
:install_clang
  if exist "c:\Program Files\LLVM\bin\clang-cl.exe" exit /b
  if not exist %CI_ROOT%\LLVM-win64.exe call :download_clang

  %_ECHO% "\e[1;33mInstalling 64-bit LLVM to 'c:\Program Files\LLVM' ...\e[0m"
  start /wait %CI_ROOT%\LLVM-win64.exe /S

  %_ECHO% "\e[1;33mDone\n--------------------------------------------------------\e[0m"
  exit /b

:download_clang
  %_ECHO% "\e[1;33mDownloading 64-bit LLVM...'.\e[0m"
  curl -# -Lo %CI_ROOT%\LLVM-win64.exe %URL_CLANG_EXE%
  if not errorlevel == 0 (
    %_ECHO% "\e[1;31mThe curl download failed!\e[0m"
    exit /b 1
  )
  exit /b

:run_tests
  cd ..\.github

  %_ECHO% "\e[1;33m\nRunning '..\dump1090 -VV':\e[0m"
  ..\dump1090 -VV

  %_ECHO% "\e[1;33m\nRunning '..\dump1090 --test aircraft,airport,net':\e[0m"
  ..\dump1090 --config dump1090.cfg --debug gn --test "aircraft,airport,net"

  %_ECHO% "\e[1;33m\nRunning 'type $(TEMP)/dump1090/reverse-resolve.csv':\e[0m"
  type %TEMP%\dump1090\reverse-resolve.csv
  exit /b

