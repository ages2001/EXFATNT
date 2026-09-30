/*
 * EXFCHK test front end (Linux): checks (and with -f repairs) an image
 * file with the same core as exfatchk.exe and exfachk.exe.
 *
 * exfchkl IMAGE [SECTOR] [-f] [-v] [-q]; the exit code is the result
 * (0 clean, 1 fixed, 2 problems left, 3 failed).
 */

#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../chk/exfchk.h"

static int Fd, Quiet;
static unsigned long Reads, Writes;

static int ReadImage(void *c, EXU64 Offset, void *Buffer, EXU32 Length)
{
    EXU32 Sector = *(EXU32 *)c;
    if (Sector && (Offset % Sector || Length % Sector)) { fprintf(stderr, "unaligned read %llu+%lu\n", Offset, (unsigned long)Length); abort(); }
    Reads++;
    return pread(Fd, Buffer, Length, (off_t)Offset) == (ssize_t)Length;
}

static int WriteImage(void *c, EXU64 Offset, const void *Buffer, EXU32 Length)
{
    EXU32 Sector = *(EXU32 *)c;
    if (Sector && (Offset % Sector || Length % Sector)) { fprintf(stderr, "unaligned write %llu+%lu\n", Offset, (unsigned long)Length); abort(); }
    Writes++;
    return pwrite(Fd, Buffer, Length, (off_t)Offset) == (ssize_t)Length;
}

static void *AllocMem(void *c, EXU32 Size) { return calloc(1, Size); }
static void FreeMem(void *c, void *p) { free(p); }

static void Print(void *c, const EXU16 *Text, EXU32 Length)
{
    EXU32 i;
    if (Quiet) return;
    for (i = 0; i < Length; i++) {
        unsigned u = Text[i];
        if (u >= 0xD800 && u < 0xDC00 && i + 1 < Length) {
            u = 0x10000 + ((u - 0xD800) << 10) + (Text[++i] - 0xDC00);
        }
        if (u < 0x80) putchar(u);
        else if (u < 0x800) { putchar(0xC0 | (u >> 6)); putchar(0x80 | (u & 0x3F)); }
        else if (u < 0x10000) { putchar(0xE0 | (u >> 12)); putchar(0x80 | ((u >> 6) & 0x3F)); putchar(0x80 | (u & 0x3F)); }
        else { putchar(0xF0 | (u >> 18)); putchar(0x80 | ((u >> 12) & 0x3F)); putchar(0x80 | ((u >> 6) & 0x3F)); putchar(0x80 | (u & 0x3F)); }
    }
    putchar('\n');
}

int main(int argc, char **argv)
{
    EXC_PARAMS P; EXC_HOST H; EXC_RESULT R; struct stat st; int i, r; EXU32 Sector = 0;
    static const char *Names[] = { "clean", "fixed", "problems left", "failed" };
    if (argc < 2) { fprintf(stderr, "exfchkl IMAGE [SECTOR] [-f] [-v] [-q]\n"); return 4; }
    memset(&P, 0, sizeof(P));
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-f")) P.Fix = 1;
        else if (!strcmp(argv[i], "-v")) P.Verbose = 1;
        else if (!strcmp(argv[i], "-q")) Quiet = 1;
        else Sector = (EXU32)atoi(argv[i]);
    }
    Fd = open(argv[1], P.Fix ? O_RDWR : O_RDONLY);
    if (Fd < 0 || fstat(Fd, &st) != 0) { perror(argv[1]); return 4; }
    P.SectorSize = Sector;
    P.VolumeSectors = Sector ? (EXU64)st.st_size / Sector : 0;
    memset(&H, 0, sizeof(H));
    H.Context = &P.SectorSize; H.Read = ReadImage; H.Write = WriteImage; H.Alloc = AllocMem; H.Free = FreeMem; H.Print = Print;
    r = ExcCheck(&P, &H, &R);
    if (!Quiet)
        printf("result: %s; %lu problems, %lu fixed; %lu files, %lu directories, %lu of %lu clusters used; %lu reads, %lu writes\n",
               Names[r], (unsigned long)R.Problems, (unsigned long)R.Fixed, (unsigned long)R.Files, (unsigned long)R.Directories,
               (unsigned long)R.UsedClusters, (unsigned long)R.ClusterCount, Reads, Writes);
    close(Fd);
    return r;
}
