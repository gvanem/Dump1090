@echo off
setlocal
set RTLSDR_TRACE=

if %_cmdproc. == 4NT. .or. %_cmdproc. == TCC. on break quit
start py.exe -3 %~dp0\tools\SBS_client.py --wait 5 --host localhost --port 30003 SBS
%~dp0\dump1090.exe --net --interactive


