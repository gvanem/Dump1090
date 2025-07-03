::
:: Get SBS data from ADSBHUB.org.
:: Ref: https://www.adsbhub.org/setting.php
::
setlocal
on break quit
set WSOCK_TRACE_LEVEL=1

%~dp0\dump1090.exe --net-active --debug GNM --config %~dp0\adsbhub.org.cfg %$


