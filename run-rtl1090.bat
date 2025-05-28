::
:: Setup for Dump1090 and "RTL1090 V3 Scope" as the RAW-IN feeder.
:: Assume it's running when invoking this .bat file.
:: Or use option '--start-rtl1090'.
::
@echo off
setlocal
if %_cmdproc. == 4NT. on break quit
if %_cmdproc. == TCC. on break quit

set MODE=
set DEBUG= --debug gn
set MY_DIR= %~dp0
set DUMP1090= %MY_DIR%\dump1090.exe

::
:: Change this; the full or relative path to your 'rtl1090.exe' program.
::
set RTL1090= %MY_DIR%\..\Flight-Software\JetVision\scope\rtl1090.beta3.exe

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
  :: --debug Gn
  set MODE= --raw
  set CONFIG= --config %MY_DIR%\host-sbs.cfg
  del /Q %MY_DIR%\host-sbs.log 2> NUL
) else (
  set CONFIG= --config %MY_DIR%\host-raw.cfg
  del /Q %MY_DIR%\host-raw.log 2> NUL
)

if %1. == --interactive. (
  shift
  set MODE= --interactive
  set DEBUG=
)

@echo on
%DUMP1090% --net-active %DEBUG% %CONFIG% %MODE% %1 %2 %3

