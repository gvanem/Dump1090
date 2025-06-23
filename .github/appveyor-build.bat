@echo off
setlocal EnableDelayedExpansion
prompt $P$G

::
:: Download-links for clang used by 'build_src' on AppVeyor or for local testing if this .bat-file.
:: These can be rather slow:
::
set URL_CLANG_EXE=https://github.com/llvm/llvm-project/releases/download/llvmorg-20.1.7/LLVM-20.1.7-win64.exe

::
:: 'APPVEYOR_PROJECT_NAME=Dump1090' unless testing this as:
::   c:\dev\Dump1090 cmd /c .github\appveyor-script.bat build_src
::
:: locally.
::
:: Change this for an 'echo.exe' with colour support. Like Cygwin.
::
set _ECHO=%CYGWIN_ROOT%\bin\echo.exe -e

if %APPVEYOR_PROJECT_NAME%. == . (
  set LOCAL_TEST=1

  if %BUILDER%. == . (
    echo BUILDER not set!
    exit /b 1
  )
  set APPVEYOR_BUILD_FOLDER=%CD%\..

) else (
  set LOCAL_TEST=0
  set APPVEYOR_BUILD_FOLDER=c:\projects\Dump1090
  set _ECHO=c:\msys64\usr\bin\echo.exe -e
)

::
:: Download stuff to here:
::
set CI_ROOT=%APPVEYOR_BUILD_FOLDER%\CI-temp
md %CI_ROOT% 2> NUL

%_ECHO% "\n\e[1;33mBuilding for 'BUILDER=%BUILDER%'.\e[0m"

set 7Z=7z x -y

if %LOCAL_TEST% == 1 goto local_test_1

::
:: Shit for brains 'cmd' cannot have this inside a 'if x (' block since
:: on a AppVeyor build several "c:\Program Files (x86)\Microsoft xxx" strings
:: are in the 'PATH'.
::
:: This is the PATH to the 64-bit 'clang-cl' already on AppVeyor.
:: set PATH=%PATH%;c:\Program Files\LLVM\bin
::
:: These are needed by 'clang-release_32.mak' and 'clang-release_64.mak'
::
set CLANG_32=c:/Program Files (x86)/LLVM
set CLANG_64=c:/Program Files/LLVM

:local_test_1

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
:: Hence cannot use a 'if x (what-ever) else (something else)' syntax with that.
::
if %LOCAL_TEST% == 1 (
  echo on
  if not exist "%APPVEYOR_BUILD_FOLDER%" (echo No '%APPVEYOR_BUILD_FOLDER%'. Edit this .bat-file & exit /b 1)
)

%_ECHO% "\e[1;33m--------------------------------------------------------------------------------------------------\e[0m"

if %BUILDER%. == visualc. (
  %_ECHO% "\e[1;33m: Building for MVC/x64:\e[0m"
  make -f Makefile.Windows CC=cl
  exit /b
)

::
:: Need to do 'call :install_CLANG' here to set the PATH for 'clang-cl.exe'!
::
if %BUILDER%. == clang. (
  cd ..
  call :install_CLANG
  cd src
  call :show_clang_info
  %_ECHO% "\e[1;33m: Building for clang-cl/x64:\e[0m"
  make -f Makefile.Windows CC=clang-cl
  exit /b
)

%_ECHO% "\e[1;31mIllegal BUILDER (BUILDER=%BUILDER%) values! Remember cmd.exe is case-sensitive.\e[0m"
exit /b 1

::
:: Download the '%CI_ROOT%\LLVM-16.0.0-win64.exe' for 64-bit 'clang-cl'.
:: A 300 MByte download which silently installs to "c:\Program Files\LLVM"
::
:install_CLANG
  if exist "c:\Program Files\LLVM\bin\clang-cl.exe" exit /b
  if exist %CD%\CI\LLVM-64-bit\bin\clang-cl.exe     exit /b

  if not exist %CI_ROOT%\LLVM-16.0.0-win64.exe call :download_CLANG

  %_ECHO% "\e[1;33mInstalling 64-bit LLVM to 'c:\Program Files\LLVM' ...\e[0m"
  start /wait %CI_ROOT%\LLVM-16.0.0-win64.exe /S

  %CD%\CI\LLVM-64-bit\bin\clang-cl -v
  %_ECHO% "\e[1;33mDone\n--------------------------------------------------------\e[0m"
  exit /b

:download_CLANG
  %_ECHO% "\e[1;33mDownloading 64-bit LLVM...'.\e[0m"
  curl -# -Lo %CI_ROOT%\LLVM-16.0.0-win64.exe %URL_CLANG_EXE%
  if not errorlevel == 0 (
    %_ECHO% "\e[1;31mThe curl download failed!\e[0m"
    exit /b 1
  )
  exit /b

::
:: Ditto for 'clang-cl'
::
:show_clang_info
  %_ECHO% "\e[1;33m: clang-cl version-info:\e[0m"
  clang-cl -v
  echo.
  exit /b

