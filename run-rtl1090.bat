::
:: Setup for Dump1090 and "RTL1090 V3 Scope" as the SBS-IN / RAW-IN feeder.
:: Assume it's running when invoking this .bat file.
:: Or use option '--start-rtl1090'.
::
:: "RTL1090 V3 Scope" can be downloaded from https://rtl1090.com/
::
@echo off
setlocal
if %_cmdproc. == 4NT. on break quit
if %_cmdproc. == TCC. on break quit

set USE_START_WINDOW=0
set MODE=
set DEBUG= --debug gn
set MY_DIR= %~dp0
set DUMP1090= %MY_DIR%\dump1090.exe

::
:: Change this; the full or relative path to your 'rtl1090.exe' program.
::
set RTL1090= %MY_DIR%\..\Flight-Software\JetVision\scope\rtl1090.beta3.exe

if %1. == --help. (
  echo Usage: %MY_DIR%run-rtl1090.bat [--window ^| --start-rtl1090 ^| --sbs ^| --interactive]
  exit /b 0
)

if %1. == --window. (
  shift
  set USE_START_WINDOW=1
)

if %1. == --start-rtl1090. (
  shift
  start %RTL1090%
  rem
  rem delay for 5 sec to let '%RTL1090%' start up
  rem
  ping.exe -4 -n 5 localhost
)

if %1. == --sbs. (
  shift
  set DEBUG=
  set MODE= --raw
  set CONFIG= --config %MY_DIR%\host-sbs.cfg
  del /Q %MY_DIR%\host-sbs.log 2> NUL
) else (
  set DEBUG= --debug Rn
  set CONFIG= --config %MY_DIR%\host-raw.cfg
  del /Q %MY_DIR%\host-raw.log 2> NUL
)

if %1. == --interactive. (
  shift
  set MODE= --interactive
  set DEBUG=
)

set FULL_CMD= %DUMP1090% --net-active %DEBUG% %CONFIG% %MODE% %1 %2 %3

if %USE_START_WINDOW%. == 1. (
  start "Run-RTL1090" %FULL_CMD%
) else (
  %FULL_CMD%
)
