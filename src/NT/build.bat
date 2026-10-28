@echo off
REM exfatnt.sys for NT 3.1, 3.5, 3.51 and 4.0. MSVCDIR = Visual C++ 4.x,
REM DDKDIR = NT4 DDK.
REM The NT4 DDK has no ntifs.h: the free one (release 58, Bo Branten,
REM http://www.acc.umu.se/~bosse/) is in this folder.
REM libcntpr.lib comes before ntoskrnl.lib so the 64-bit arithmetic helpers
REM (_allmul, _aulldiv, ...) are linked in: NT 3.x does not export them.
REM
REM Subsystem version 3.10 and -base 0x10000 so the NT 3.1 loader accepts it.
REM EXF_NT31 selects the universal profile: the executive-resource API is bound
REM to the running kernel at load time (exfres.c), so one binary loads on NT 3.1
REM (legacy resources) through 4.0 (Lite). -release writes the PE checksum the
REM NT 3.51+ loader validates.

set MSVCDIR=C:\MSDEV
set DDKDIR=C:\NT4DDK

if not exist ntifs.h echo ntifs.h not found in this folder -- see above.
if not exist ntifs.h goto error

set PATH=%MSVCDIR%\BIN;%DDKDIR%\BIN;%PATH%
set INCLUDE=.;%DDKDIR%\inc;%DDKDIR%\inc\crt;%MSVCDIR%\INCLUDE
set LIB=%DDKDIR%\lib\i386;%MSVCDIR%\LIB;%DDKDIR%\lib\i386\free;%LIB%

if exist *.obj del *.obj
if exist exfatnt.sys del exfatnt.sys

cl -nologo -c -Gz -Ox -W3 -Zp8 -Zi -Zl -DEXF_NT4 -DEXF_NT31 -D_X86_=1 -Di386=1 -DSTD_CALL -DCONDITION_HANDLING=1 -DNT_UP=0 -DNT_INST=0 -DWIN32=100 -D_NT1X_=100 -DWINNT=1 -D_WIN32_WINNT=0x0400 -DDEVL=1 -DFPO=1 /I. /I.. ..\exfalloc.c ..\exfclose.c ..\exfcreat.c ..\exfdir.c ..\exfdirw.c ..\exfdisp.c ..\exffast.c ..\exfflush.c ..\exffsctl.c ..\exfinfo.c ..\exfinit.c ..\exfio.c ..\exfmisc.c ..\exfread.c ..\exfres.c ..\exfsetin.c ..\exfstruc.c ..\exfsup.c ..\exfvol.c ..\exfwrite.c
if errorlevel 1 goto error

link -nologo -release -version:1.2 -debug -debugtype:both -subsystem:native,3.10 -entry:DriverEntry@8 -driver -base:0x10000 -align:0x200 -nodefaultlib -out:exfatnt.sys exfalloc.obj exfclose.obj exfcreat.obj exfdir.obj exfdirw.obj exfdisp.obj exffast.obj exfflush.obj exffsctl.obj exfinfo.obj exfinit.obj exfio.obj exfmisc.obj exfres.obj exfsetin.obj exfstruc.obj exfsup.obj exfvol.obj exfwrite.obj libcntpr.lib ntoskrnl.lib hal.lib
if errorlevel 1 goto error

echo exfatnt.sys built.
goto end

:error
echo Build FAILED.

:end
