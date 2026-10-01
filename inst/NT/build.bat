@echo off
REM exfinst.exe for NT 3.1 and later. MSVCDIR = Visual C++ 4.x
REM No C library (exfcrt.c from fmt\; libc.lib only for compiler helpers).

set MSVCDIR=C:\MSDEV

set PATH=%MSVCDIR%\BIN;%PATH%
set INCLUDE=%MSVCDIR%\INCLUDE
set LIB=%MSVCDIR%\LIB

if exist *.obj del *.obj
if exist exfinst.exe del exfinst.exe

cl -nologo -c -O2 -W3 -Zl -DWIN32 -D_CONSOLE -DEXF_OWN_CRT /I.. /I..\..\fmt ..\exfinst.c ..\..\fmt\exfcrt.c
if errorlevel 1 goto error

link -nologo -subsystem:console,3.10 -entry:ExfCrtStart@0 -nodefaultlib -out:exfinst.exe exfinst.obj exfcrt.obj kernel32.lib advapi32.lib libc.lib
if errorlevel 1 goto error

del *.obj
echo exfinst.exe built.
goto end

:error
echo Build FAILED.

:end
