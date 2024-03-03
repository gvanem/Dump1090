@echo off
echo Deleting '%~dp0\aircraft-database.csv' and '%~dp0\aircraft-database.csv.sqlite' to force an update ...
del %~dp0\aircraft-database.csv %~dp0\aircraft-database.csv.sqlite

%~dp0\dump1090.exe %* --update

