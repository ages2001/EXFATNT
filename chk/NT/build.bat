@echo off
REM exfatchk.exe and exfachk.exe for NT 3.51, NT 4.0 and later.
REM MSVCDIR = Visual C++ 4.x, DDKDIR = NT4 DDK (for ntdll.lib and libcntpr.lib,
REM which the boot-time exfachk.exe links with instead of the C library).
REM Subsystem version 3.10, and exfatchk.exe without the C library (exfcrt.c),
REM so that NT 3.1 and 3.51 run them too. libcntpr.lib comes
REM before ntdll.lib: the 64-bit helpers are linked in, not imported.

set MSVCDIR=C:\MSDEV
set DDKDIR=C:\NT4DDK

set PATH=%MSVCDIR%\BIN;%PATH%
set INCLUDE=%MSVCDIR%\INCLUDE
set LIB=%MSVCDIR%\LIB

if exist *.obj del *.obj
if exist exfatchk.exe del exfatchk.exe
if exist exfachk.exe del exfachk.exe

cl -nologo -c -O2 -W3 -Zl -DWIN32 -D_CONSOLE -DEXF_OWN_CRT /I.. /I..\..\fmt ..\exfatchk.c ..\exfchkc.c ..\exfupcw.c ..\..\fmt\exfcrt.c
if errorlevel 1 goto error

link -nologo -subsystem:console,3.10 -entry:ExfCrtStart@0 -nodefaultlib -out:exfatchk.exe exfatchk.obj exfchkc.obj exfupcw.obj exfcrt.obj kernel32.lib advapi32.lib libc.lib
if errorlevel 1 goto error

del *.obj

cl -nologo -c -O2 -W3 -Zl /I.. /I..\..\fmt ..\exfachk.c ..\exfchkc.c ..\exfupcw.c
if errorlevel 1 goto error

link -nologo -subsystem:native,3.10 -entry:NtProcessStartup@4 -nodefaultlib -out:exfachk.exe exfachk.obj exfchkc.obj exfupcw.obj %DDKDIR%\lib\i386\free\libcntpr.lib %DDKDIR%\lib\i386\free\ntdll.lib
if errorlevel 1 goto error

echo exfatchk.exe and exfachk.exe built.
goto end

:error
echo Build FAILED.

:end
