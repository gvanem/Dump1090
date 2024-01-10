@echo off
setlocal
set RTLSDR_TRACE=

if %_cmdproc. == 4NT. .or. %_cmdproc. == TCC. on break quit
start py.exe -3 tools/SBS_client.py --wait 5 --host localhost --port 30003 SBS
dump1090.exe --net --interactive

