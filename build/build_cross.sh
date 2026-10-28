#!/bin/sh
# EXFATNT cross build: one native exfatnt.sys for Windows NT 3.1 - 4.0 (and
# the tools and the installer) with GCC + the mingw-w64 DDK headers, no
# Visual C++. The executive-resource API, change notification and the
# per-thread I/O helpers are bound to the running kernel at load time
# (src/exfres.c), so the one binary loads on NT 3.1, 3.5, 3.51 and 4.0.
#
# Needs: gcc-mingw-w64-i686, binutils, python3.
#   NT31_EXPORTS=file  check every driver import against NT 3.1's ntoskrnl
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
SRC="$ROOT/src"
OUT="${1:-/tmp/exfatnt-cross}"
mkdir -p "$OUT"

GCC=i686-w64-mingw32-gcc
DLLTOOL=i686-w64-mingw32-dlltool
NM=i686-w64-mingw32-nm
INC="-I$SRC -I/usr/i686-w64-mingw32/include/ddk -I/usr/i686-w64-mingw32/include"
# -march=i486: NT 3.1 runs on 80486 (and 386) CPUs; no P6 instructions.
FLAGS="-c -O2 -fno-strict-aliasing -ffreestanding -fno-stack-protector -fno-builtin -march=i486 -mtune=i486 \
 -D_X86_ -DWIN32 -D_WIN32_WINNT=0x0600 -DNTDDI_VERSION=0x06000000 -DEXF_CROSS -w $EXTRACFLAGS"

echo "Compiling driver sources..."
rm -f "$OUT"/*.o
for f in "$SRC"/exf*.c; do
    $GCC $FLAGS $INC "$f" -o "$OUT/$(basename "${f%.c}").o"
done
$GCC $FLAGS $INC "$HERE/exfcrt.c" -o "$OUT/exfcrt.o"

echo "Building the ntoskrnl import library from the symbols used..."
$NM "$OUT"/*.o 2>/dev/null | awk '/ U /{print $2}' | grep '^__imp_' \
    | sed 's/^__imp_//; s/^_//; s/@[0-9]*$//' | sort -u > "$OUT/need.txt"
if [ -f "$NT31_EXPORTS" ]; then
    echo "Checking imports against NT 3.1 ntoskrnl..."
    for s in $(cat "$OUT/need.txt"); do
        grep -qx "$s" "$NT31_EXPORTS" || echo "  NOT EXPORTED BY NT 3.1: $s"
    done
fi
$NM "$OUT"/*.o 2>/dev/null | awk '/ U /{print $2}' | grep '^__imp_' \
    | sed 's/^__imp_//; s/^_//' | sort -u > "$OUT/needat.txt"
{ echo "LIBRARY ntoskrnl.exe"; echo "EXPORTS"; cat "$OUT/needat.txt"; } > "$OUT/ntoskrnl.def"
$DLLTOOL --kill-at -d "$OUT/ntoskrnl.def" -l "$OUT/libntoskrnl_nt31.a"

echo "Linking exfatnt.sys (native, subsystem 3.10, base 0x10000)..."
$GCC -shared -nostdlib -nostartfiles \
    -Wl,--subsystem,native -Wl,--major-subsystem-version,3 -Wl,--minor-subsystem-version,10 \
    -Wl,--major-os-version,1 -Wl,--minor-os-version,0 \
    -Wl,--major-image-version,1 -Wl,--minor-image-version,2 \
    -Wl,--image-base,0x10000 -Wl,--entry,_DriverEntry@8 -Wl,--file-alignment,0x200 \
    -o "$OUT/exfatnt.sys" "$OUT"/*.o "$OUT/libntoskrnl_nt31.a" -lgcc
# Strip and checksum: the NT 3.51+ loader rejects a driver with trailing
# symbols or a wrong PE checksum as "missing or corrupt".
i686-w64-mingw32-strip --strip-all "$OUT/exfatnt.sys"
python3 "$HERE/pechecksum.py" "$OUT/exfatnt.sys"
echo "Built: $OUT/exfatnt.sys"

# ----------------------------------------------------------------------
# The tools: exfinst, exfmt, exfatchk (console) and exfachk (native, the
# boot-time check). Freestanding (subsystem 3.10), so they run on NT 3.1
# with no C runtime DLL; build/nocrt.c and build/toolcrt.c are the tiny
# built-in runtime.
# ----------------------------------------------------------------------
EXEBASE="-O2 -w -ffreestanding -nostdlib -nostartfiles -march=i486 -mtune=i486 -e _mainCRTStartup \
 -Wl,--subsystem,console -Wl,--major-subsystem-version,3 -Wl,--minor-subsystem-version,10 \
 -Wl,--major-os-version,1 -Wl,--minor-os-version,0 \
 -Wl,--major-image-version,1 -Wl,--minor-image-version,2 -D_WIN32 -D_CONSOLE"
TOOLCRT="-include $HERE/toolcrt.h \
 -D_INC_STDIO -D_INC_STDLIB -D_INC_STRING -D_INC_CTYPE -D_INC_TIME \
 -D_STDIO_H_=1 -D_STDLIB_H=1 -D_STRING_H_=1 -D_CTYPE_H_=1 -D_TIME_H_=1 \
 -Dmain=appmain -Dprintf=exf_printf -Dfprintf=exf_fprintf -Dsprintf=exf_sprintf -Dsnprintf=exf_snprintf \
 -Dmalloc=exf_malloc -Dfree=exf_free -Dcalloc=exf_calloc -Drealloc=exf_realloc \
 -Dstrtoul=exf_strtoul -Dtime=exf_time -Dlocaltime=exf_localtime -Dtoupper=exf_toupper -D_stricmp=exf_stricmp"

echo "Linking exfinst.exe..."
$GCC $EXEBASE $TOOLCRT -o "$OUT/exfinst.exe" "$ROOT/inst/exfinst.c" "$HERE/toolcrt.c" -ladvapi32 -lkernel32 -lgcc
echo "Linking exfmt.exe..."
$GCC $EXEBASE $TOOLCRT -I"$ROOT/fmt" -o "$OUT/exfmt.exe" \
    "$ROOT/fmt/exfmt.c" "$ROOT/fmt/exfmtc.c" "$ROOT/fmt/exfupc.c" "$HERE/toolcrt.c" -lkernel32 -lgcc
echo "Linking exfatchk.exe..."
$GCC $EXEBASE $TOOLCRT -I"$ROOT/chk" -I"$ROOT/fmt" -o "$OUT/exfatchk.exe" \
    "$ROOT/chk/exfatchk.c" "$ROOT/chk/exfchkc.c" "$ROOT/chk/exfupcw.c" "$HERE/toolcrt.c" "$HERE/wcrt.c" -ladvapi32 -lkernel32 -lgcc
echo "Linking exfachk.exe (native)..."
$GCC -O2 -w -ffreestanding -nostdlib -nostartfiles -fno-builtin -march=i486 -mtune=i486 -e _NtProcessStartup@4 \
    -Wl,--subsystem,native -Wl,--major-subsystem-version,3 -Wl,--minor-subsystem-version,10 \
    -Wl,--major-os-version,1 -Wl,--minor-os-version,0 -Wl,--major-image-version,1 -Wl,--minor-image-version,2 \
    -I"$ROOT/chk" -I"$ROOT/fmt" -o "$OUT/exfachk.exe" \
    "$ROOT/chk/exfachk.c" "$ROOT/chk/exfchkc.c" "$ROOT/chk/exfupcw.c" "$HERE/exfcrt.c" -lntdll -lgcc
for f in exfinst exfmt exfatchk exfachk; do python3 "$HERE/pechecksum.py" "$OUT/$f.exe" >/dev/null; done
echo "Done."
