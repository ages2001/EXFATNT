@echo off
REM exfmt.exe for NT 3.51, NT 4.0 and later. MSVCDIR = Visual C++ 4.x
REM Only the compiler and the Win32 headers are needed, not the DDK.
REM Subsystem version 3.10, and no C library (exfcrt.c instead; libc.lib only
REM for the 64-bit arithmetic helpers), so that NT 3.1 and 3.51 run it too.

set MSVCDIR=C:\MSDEV

set PATH=%MSVCDIR%\BIN;%PATH%
set INCLUDE=%MSVCDIR%\INCLUDE
set LIB=%MSVCDIR%\LIB

if exist *.obj del *.obj
if exist exfmt.exe del exfmt.exe

cl -nologo -c -O2 -W3 -Zl -DWIN32 -D_CONSOLE -DEXF_OWN_CRT /I.. ..\exfmt.c ..\exfmtc.c ..\exfupc.c ..\exfcrt.c
if errorlevel 1 goto error

link -nologo -subsystem:console,3.10 -entry:ExfCrtStart@0 -nodefaultlib -out:exfmt.exe exfmt.obj exfmtc.obj exfupc.obj exfcrt.obj kernel32.lib libc.lib
if errorlevel 1 goto error

echo exfmt.exe built.
goto end

:error
echo Build FAILED.

:end
