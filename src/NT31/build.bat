@echo off
REM exfatnt.sys for Windows NT 3.1. MSVCDIR = Visual C++ 4.x, DDKDIR = NT4 DDK
REM Built like the NT 3.51/4.0 driver (ntifs.h from ..\NT), with EXF_NT31:
REM see ..\exfnt31.h. The NT 3.1 DDK is not needed. -release: NT 3.1 loads
REM a system driver only with a valid image checksum.

set MSVCDIR=C:\MSDEV
set DDKDIR=C:\NT4DDK

if not exist ..\NT\ntifs.h echo ..\NT\ntifs.h not found.
if not exist ..\NT\ntifs.h goto error

set PATH=%MSVCDIR%\BIN;%DDKDIR%\BIN;%PATH%
set INCLUDE=.;..\NT;%DDKDIR%\inc;%DDKDIR%\inc\crt;%MSVCDIR%\INCLUDE
set LIB=%DDKDIR%\lib\i386;%MSVCDIR%\LIB;%DDKDIR%\lib\i386\free;%LIB%

if exist *.obj del *.obj
if exist exfatnt.sys del exfatnt.sys

cl -nologo -c -Gz -Ox -W3 -Zp8 -Zi -Zl -DEXF_NT31 -D_X86_=1 -Di386=1 -DSTD_CALL -DCONDITION_HANDLING=1 -DNT_UP=0 -DNT_INST=0 -DWIN32=100 -D_NT1X_=100 -DWINNT=1 -D_WIN32_WINNT=0x0400 -DDEVL=1 -DFPO=1 /I. /I..\NT /I.. ..\exfalloc.c ..\exfclose.c ..\exfcreat.c ..\exfdir.c ..\exfdirw.c ..\exfdisp.c ..\exffast.c ..\exfflush.c ..\exffsctl.c ..\exfinfo.c ..\exfinit.c ..\exfio.c ..\exfmisc.c ..\exfread.c ..\exfsetin.c ..\exfstruc.c ..\exfsup.c ..\exfvol.c ..\exfwrite.c
if errorlevel 1 goto error

link -nologo -release -debug -debugtype:both -subsystem:native,3.10 -entry:DriverEntry@8 -driver -base:0x10000 -align:0x200 -nodefaultlib -out:exfatnt.sys exfalloc.obj exfclose.obj exfcreat.obj exfdir.obj exfdirw.obj exfdisp.obj exffast.obj exfflush.obj exffsctl.obj exfinfo.obj exfinit.obj exfio.obj exfmisc.obj exfread.obj exfsetin.obj exfstruc.obj exfsup.obj exfvol.obj exfwrite.obj libcntpr.lib ntoskrnl.lib hal.lib
if errorlevel 1 goto error

echo exfatnt.sys for NT 3.1 built.
goto end

:error
echo Build FAILED.

:end
