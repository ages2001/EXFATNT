/*
 * EXFMT test front end (Linux): formats an image file with the same core
 * as exfmt.exe.
 *
 * exfmtl IMAGE SECTOR [-c CLUSTER] [-L LABEL] [-p PARTITIONOFFSET] [-s SERIAL] [-f] [-q]
 */

#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../fmt/exfmt.h"

static int Fd;
static EXU64 Writes, Bytes;

static int WriteImage(void *Context, EXU64 Offset, const void *Buffer, EXU32 Length)
{
    EXU32 Sector = *(EXU32 *)Context;
    if (Offset % Sector || Length % Sector) {
        fprintf(stderr, "unaligned write %llu+%lu\n", Offset, (unsigned long)Length);
        return 0;
    }
    Writes++; Bytes += Length;
    return pwrite(Fd, Buffer, Length, (off_t)Offset) == (ssize_t)Length;
}

/* UTF-8 to UTF-16, surrogate pairs included */
static EXU32 Utf16(const char *s, EXU16 *out, EXU32 max)
{
    const unsigned char *p = (const unsigned char *)s; EXU32 n = 0, c;
    while (*p) {
        if (*p < 0x80) c = *p++;
        else if (*p < 0xE0) { c = (*p++ & 0x1F) << 6; c |= *p++ & 0x3F; }
        else if (*p < 0xF0) { c = (*p++ & 0x0F) << 12; c |= (*p++ & 0x3F) << 6; c |= *p++ & 0x3F; }
        else { c = (*p++ & 0x07) << 18; c |= (*p++ & 0x3F) << 12; c |= (*p++ & 0x3F) << 6; c |= *p++ & 0x3F; }
        if (c >= 0x10000) {
            if (n + 2 > max) return max + 1;
            c -= 0x10000; out[n++] = (EXU16)(0xD800 + (c >> 10)); out[n++] = (EXU16)(0xDC00 + (c & 0x3FF));
        } else {
            if (n + 1 > max) return max + 1;
            out[n++] = (EXU16)c;
        }
    }
    return n;
}

int main(int argc, char **argv)
{
    EXFMT_PARAMS P; EXFMT_LAYOUT L; struct stat st; int i, r, quiet = 0;
    if (argc < 3) { fprintf(stderr, "exfmtl IMAGE SECTOR [-c CLUSTER] [-L LABEL] [-p OFFSET] [-s SERIAL] [-f] [-q]\n"); return 2; }
    memset(&P, 0, sizeof(P));
    P.SectorSize = (EXU32)atoi(argv[2]);
    P.Serial = 0x12345678;
    for (i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) P.ClusterSize = (EXU32)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-L") && i + 1 < argc) {
            EXU16 tmp[64];
            P.LabelLength = Utf16(argv[++i], tmp, 60);
            if (P.LabelLength > EXFMT_MAX_LABEL) { fprintf(stderr, "exfmtl: %s\n", ExfmtMessage(EXFMT_BAD_LABEL)); return 1; }
            memcpy(P.Label, tmp, P.LabelLength * 2);
        }
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) P.PartitionOffset = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) P.Serial = (EXU32)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-f")) P.Full = 1;
        else if (!strcmp(argv[i], "-q")) quiet = 1;
        else { fprintf(stderr, "exfmtl: bad argument %s\n", argv[i]); return 2; }
    }
    Fd = open(argv[1], O_RDWR);
    if (Fd < 0 || fstat(Fd, &st) != 0) { perror(argv[1]); return 1; }
    P.VolumeSectors = (EXU64)st.st_size / (P.SectorSize ? P.SectorSize : 1);
    r = ExfmtLayout(&P, &L);
    if (r != EXFMT_OK) { fprintf(stderr, "exfmtl: %s\n", ExfmtMessage(r)); return 1; }
    if (!quiet)
        printf("sectors %llu x %lu, cluster %lu, FAT at %lu (%lu sectors), heap at %lu, %lu clusters, bitmap %lu+%lu, up-case %lu+%lu, root %lu, %lu%% used\n",
               L.VolumeSectors, (unsigned long)L.SectorSize, (unsigned long)L.ClusterSize, (unsigned long)L.FatOffset, (unsigned long)L.FatLength,
               (unsigned long)L.HeapOffset, (unsigned long)L.ClusterCount, (unsigned long)L.BitmapCluster, (unsigned long)L.BitmapClusters,
               (unsigned long)L.UpcaseCluster, (unsigned long)L.UpcaseClusters, (unsigned long)L.RootCluster, (unsigned long)L.PercentInUse);
    r = ExfmtFormat(&P, &L, WriteImage, NULL, &P.SectorSize);
    if (r != EXFMT_OK) { fprintf(stderr, "exfmtl: %s\n", ExfmtMessage(r)); return 1; }
    if (!quiet) printf("%llu writes, %llu bytes\n", Writes, Bytes);
    close(Fd);
    return 0;
}
