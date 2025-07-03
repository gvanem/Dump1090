@echo off
setlocal
if %_cmdproc%. == 4NT. .or. %_cmdproc%. == TCC. (on break quit)

set SQL_SCRIPT=c:\\temp\\dump1090\\aircraft-database-dump.sql
set   SQL_DUMP=c:\\temp\\dump1090\\aircraft-database.dump

mkdir %TEMP%\dump1090 2> NUL

echo Generating '%SQL_DUMP%' of 'H1P' type aircrafts via '%SQL_SCRIPT%'

echo .output %SQL_DUMP%                           > %SQL_SCRIPT%
echo .separator ,                                >> %SQL_SCRIPT%
echo select * from aircrafts where type == "H1P" >> %SQL_SCRIPT%

%~dp0\sqlite3.exe %~dp0\aircraft-database.csv.sqlite < %SQL_SCRIPT%
