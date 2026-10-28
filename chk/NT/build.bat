@echo off
REM exfatchk.exe and exfachk.exe for NT 3.51, NT 4.0 and later.
REM MSVCDIR = Visual C++ 4.x, DDKDIR = NT4 DDK (for ntdll.lib and libcntpr.lib,
REM which the boot-time exfachk.exe links with instead of the C library).
REM Subsystem version 3.10 so that NT 3.51 loads them too.

set MSVCDIR=C:\MSDEV
set DDKDIR=C:\NT4DDK

set PATH=%MSVCDIR%\BIN;%PATH%
set INCLUDE=%MSVCDIR%\INCLUDE
set LIB=%MSVCDIR%\LIB

if exist *.obj del *.obj
if exist exfatchk.exe del exfatchk.exe
if exist exfachk.exe del exfachk.exe

cl -nologo -c -O2 -W3 -ML -DWIN32 -D_CONSOLE /I.. /I..\..\fmt ..\exfatchk.c ..\exfchkc.c ..\exfupcw.c
if errorlevel 1 goto error

link -nologo -subsystem:console,3.10 -out:exfatchk.exe exfatchk.obj exfchkc.obj exfupcw.obj kernel32.lib advapi32.lib
if errorlevel 1 goto error

del *.obj

cl -nologo -c -O2 -W3 -Zl /I.. /I..\..\fmt ..\exfachk.c ..\exfchkc.c ..\exfupcw.c
if errorlevel 1 goto error

link -nologo -subsystem:native,3.10 -entry:NtProcessStartup@4 -nodefaultlib -out:exfachk.exe exfachk.obj exfchkc.obj exfupcw.obj %DDKDIR%\lib\i386\free\ntdll.lib %DDKDIR%\lib\i386\free\libcntpr.lib
if errorlevel 1 goto error

echo exfatchk.exe and exfachk.exe built.
goto end

:error
echo Build FAILED.

:end
