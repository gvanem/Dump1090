::
:: Capture some Web-traffic to/from port 8080.
:: Needs NPcap, windump and a tee.exe program (from Msys/Cygwin)
::
@echo off
setlocal
set LOG_FILE= %~dp0\port-8080.log
set NUM=50
set PCAP_TRACE=
set WSOCK_TRACE_LEVEL=

if %_cmdproc. == 4NT. .or. %_cmdproc. == TCC. (
  set _echo=echo_yellow
  on break (%_echo Quitting %+ quit)
) else (
  set _echo=echo
)

%_echo% Capturing %NUM% packets of Loopback traffic on port 8080 ...

windump.exe -i\Device\NPF_Loopback -#tvc %NUM% port 8080 | tee.exe %LOG_FILE%

