/*
 * EXFCHK - check and repair of an exFAT volume
 *
 * The order is the one of the on-disk structures: boot region and its
 * backup, the FAT's first entries, the root directory with the allocation
 * bitmap and the up-case table, then every directory breadth first, each
 * entry set and its cluster chain. Every cluster reached is marked; a
 * cluster reached twice is a cross link and the later owner is cut short.
 * Last the allocation bitmap is compared with the clusters marked, and the
 * volume flags are cleared once nothing is left to fix.
 *
 * Repairs only ever shorten or drop what cannot be trusted: a broken chain
 * ends at its last good cluster, a size is cut to the clusters there are,
 * an entry set that cannot be read is deleted. Nothing is written unless
 * Fix is set, and all writes go through a block cache flushed at the end,
 * the boot sector last.
 */

#include <stdarg.h>
#include <stddef.h>
#include "exfchk.h"

#define BLOCK_SIZE      65536UL
#define BLOCK_COUNT     64
#define BUCKETS         128
#define NAME_BUCKETS    1024
#define EOC             0xFFFFFFFFUL
#define BAD_CLUSTER     0xFFFFFFF7UL
#define MAX_PATH_CHARS  2048

/* Chain walk results */
#define CH_OK       0
#define CH_BAD      1   /* a cluster number out of the heap */
#define CH_CROSS    2   /* a cluster already used by something else (or a loop) */
#define CH_SHORT    3   /* ends before its size says */
#define CH_LONG     4   /* goes on past its size */

typedef struct _BLK {
    EXU64 Base;
    EXU32 Length;
    EXU32 DirtyFrom;
    EXU32 DirtyTo;
    EXU32 Age;
    int InUse;
    EXU8 *Data;
    struct _BLK *Next;
} BLK;

typedef struct _DIRQ {
    EXU32 First;
    EXU32 Clusters;
    int NoFat;
    EXU16 *Path;
    EXU32 PathLength;
} DIRQ;

typedef struct _NAMEREC {
    EXU32 Hash;
    EXU32 Length;
    EXU32 Offset;
    EXU32 Next;
} NAMEREC;

typedef struct _CHAIN {
    EXU32 Good;         /* clusters that can be kept */
    EXU32 Last;         /* the last of them */
    int Problem;
    EXU32 At;           /* the cluster the problem is at */
} CHAIN;

typedef struct _VOL {
    const EXC_PARAMS *P;
    const EXC_HOST *H;
    EXC_RESULT *R;
    int Fix;
    int Failed;

    EXU32 SectorSize;
    EXU32 SectorShift;
    EXU32 ClusterSize;
    EXU32 ClusterShift;         /* bytes, as a shift */
    EXU64 VolumeBytes;
    EXU64 FatByte;
    EXU32 FatLength;
    EXU64 HeapByte;
    EXU32 ClusterCount;
    EXU32 RootCluster;
    EXU32 NumberOfFats;
    EXU32 VolumeFlags;
    EXU32 PercentInUse;

    BLK Blocks[BLOCK_COUNT];
    BLK *Hash[BUCKETS];
    EXU32 Clock;

    EXU8 *Used;                 /* bit n: cluster n + 2 */
    EXU8 *Heads;                /* first clusters of every intact entry set */
    EXU16 *Upcase;              /* 65536 entries */

    int HaveBitmap;
    EXU32 *BitmapList;
    EXU32 BitmapClusters;
    EXU64 BitmapLength;

    DIRQ *Queue;
    EXU32 QueueCount;
    EXU32 QueueCap;

    NAMEREC *Names;
    EXU32 NameCount;
    EXU32 NameCap;
    EXU16 *Arena;
    EXU32 ArenaUsed;
    EXU32 ArenaCap;
    EXU32 NameBuckets[NAME_BUCKETS];

    const EXU16 *DirPath;       /* for messages */
    EXU32 DirPathLength;

    int NeedUpcase;             /* to be made again once the free clusters are known */
    int NeedBitmap;
    int FreshBitmap;            /* written anew: its old contents mean nothing */
    EXU32 *RootList;
    EXU32 RootClusters;
} VOL;

/* ------------------------------------------------------------------ */
/* Small helpers (no C library)                                        */
/* ------------------------------------------------------------------ */

static void Zero(void *p, EXU32 n) { EXU8 *d = (EXU8 *)p; while (n--) *d++ = 0; }
static void Copy(void *to, const void *from, EXU32 n)
{
    EXU8 *d = (EXU8 *)to; const EXU8 *s = (const EXU8 *)from;
    while (n--) *d++ = *s++;
}

static EXU32 Get16(const EXU8 *p) { return (EXU32)p[0] | ((EXU32)p[1] << 8); }
static EXU32 Get32(const EXU8 *p) { return Get16(p) | (Get16(p + 2) << 16); }
static EXU64 Get64(const EXU8 *p) { return (EXU64)Get32(p) | ((EXU64)Get32(p + 4) << 32); }
static void Put16(EXU8 *p, EXU32 v) { p[0] = (EXU8)v; p[1] = (EXU8)(v >> 8); }
static void Put32(EXU8 *p, EXU32 v) { Put16(p, v & 0xFFFF); Put16(p + 2, v >> 16); }
static void Put64(EXU8 *p, EXU64 v) { Put32(p, (EXU32)(v & 0xFFFFFFFFUL)); Put32(p + 4, (EXU32)(v >> 32)); }

static void *Alloc(VOL *V, EXU32 Size)
{
    void *p = V->H->Alloc(V->H->Context, Size ? Size : 1);
    if (p == NULL) {
        V->Failed = 1;
    }
    return p;
}

static void Free(VOL *V, void *p)
{
    if (p != NULL) {
        V->H->Free(V->H->Context, p);
    }
}

/* Makes room for Need elements; 0 when memory ran out */
static int Grow(VOL *V, void **Array, EXU32 *Cap, EXU32 Need, EXU32 Elem)
{
    EXU32 NewCap;
    void *New;

    if (Need <= *Cap) {
        return 1;
    }
    NewCap = *Cap ? *Cap : 64;
    while (NewCap < Need) {
        NewCap *= 2;
    }
    New = Alloc(V, NewCap * Elem);
    if (New == NULL) {
        return 0;
    }
    if (*Array != NULL) {
        Copy(New, *Array, *Cap * Elem);
        Free(V, *Array);
    }
    *Array = New;
    *Cap = NewCap;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Output                                                              */
/* ------------------------------------------------------------------ */

typedef struct _OUT {
    EXU16 Text[600];
    EXU32 Length;
} OUT;

static void OutChar(OUT *o, EXU32 c)
{
    if (o->Length < sizeof(o->Text) / sizeof(o->Text[0])) {
        o->Text[o->Length++] = (EXU16)c;
    }
}

static void OutAscii(OUT *o, const char *s)
{
    while (*s) {
        OutChar(o, (EXU8)*s++);
    }
}

static void OutUnits(OUT *o, const EXU16 *s, EXU32 n)
{
    EXU32 i;
    for (i = 0; i < n; i++) {
        OutChar(o, s[i]);
    }
}

static void OutNumber(OUT *o, EXU64 v)
{
    char d[24];
    int n = 0;
    do {
        d[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    } while (v != 0);
    while (n > 0) {
        OutChar(o, (EXU8)d[--n]);
    }
}

/*
 * %s ASCII, %u EXU32, %q EXU64, %n name (EXU16 *, EXU32 length),
 * %p the directory being checked
 */
static void Format(VOL *V, OUT *o, const char *f, va_list ap)
{
    for (; *f; f++) {
        if (*f != '%') {
            OutChar(o, (EXU8)*f);
            continue;
        }
        switch (*++f) {
        case 's': OutAscii(o, va_arg(ap, const char *)); break;
        case 'u': OutNumber(o, va_arg(ap, EXU32)); break;
        case 'q': OutNumber(o, va_arg(ap, EXU64)); break;
        case 'n': {
            const EXU16 *s = va_arg(ap, const EXU16 *);
            EXU32 n = va_arg(ap, EXU32);
            OutUnits(o, s, n);
            break;
        }
        case 'p':
            if (V->DirPathLength == 0) {
                /* The root: "\" alone, nothing before "\name" */
                if (f[1] != '\\') {
                    OutChar(o, '\\');
                }
            } else {
                OutUnits(o, V->DirPath, V->DirPathLength);
            }
            break;
        case '%': OutChar(o, '%'); break;
        default: f--; break;
        }
    }
}

static void Say(VOL *V, const char *f, ...)
{
    OUT o;
    va_list ap;
    o.Length = 0;
    va_start(ap, f);
    Format(V, &o, f, ap);
    va_end(ap);
    V->H->Print(V->H->Context, o.Text, o.Length);
}

/*
 * One problem: said, counted, and TRUE when the caller is to fix it.
 * Fixable says whether this program knows how.
 */
static int Problem(VOL *V, int Fixable, const char *f, ...)
{
    OUT o;
    va_list ap;
    int Fix = V->Fix && Fixable;

    o.Length = 0;
    va_start(ap, f);
    Format(V, &o, f, ap);
    va_end(ap);

    if (Fix) {
        OutAscii(&o, " Fixed.");
        V->R->Fixed++;
    } else if (V->Fix) {
        OutAscii(&o, " Not fixed.");
    }

    V->R->Problems++;
    V->H->Print(V->H->Context, o.Text, o.Length);
    return Fix;
}

/* ------------------------------------------------------------------ */
/* Block cache                                                         */
/* ------------------------------------------------------------------ */

static int FlushBlock(VOL *V, BLK *b)
{
    EXU32 From, To;

    if (b->DirtyTo <= b->DirtyFrom) {
        return 1;
    }
    From = b->DirtyFrom & ~(V->SectorSize - 1);
    To = (b->DirtyTo + V->SectorSize - 1) & ~(V->SectorSize - 1);
    if (To > b->Length) {
        To = b->Length;
    }
    b->DirtyFrom = b->DirtyTo = 0;
    if (!V->H->Write(V->H->Context, b->Base + From, b->Data + From, To - From)) {
        Say(V, "Writing to the volume at byte %q failed.", b->Base + From);
        V->Failed = 1;
        return 0;
    }
    return 1;
}

static int FlushAll(VOL *V)
{
    int i, Ok = 1;
    for (i = 0; i < BLOCK_COUNT; i++) {
        if (V->Blocks[i].InUse && !FlushBlock(V, &V->Blocks[i])) {
            Ok = 0;
        }
    }
    return Ok;
}

static BLK *GetBlock(VOL *V, EXU64 Offset)
{
    EXU64 Base = Offset & ~(EXU64)(BLOCK_SIZE - 1);
    EXU32 Bucket = (EXU32)((Base / BLOCK_SIZE) % BUCKETS);
    BLK *b, **pp, *Victim = NULL;
    int i;

    if (V->Failed || Offset >= V->VolumeBytes) {
        V->Failed = 1;
        return NULL;
    }

    for (b = V->Hash[Bucket]; b != NULL; b = b->Next) {
        if (b->Base == Base) {
            b->Age = ++V->Clock;
            return b;
        }
    }

    for (i = 0; i < BLOCK_COUNT; i++) {
        b = &V->Blocks[i];
        if (!b->InUse) {
            Victim = b;
            break;
        }
        if (Victim == NULL || b->Age < Victim->Age) {
            Victim = b;
        }
    }

    b = Victim;
    if (b->InUse) {
        if (!FlushBlock(V, b)) {
            return NULL;
        }
        pp = &V->Hash[(EXU32)((b->Base / BLOCK_SIZE) % BUCKETS)];
        while (*pp != b) {
            pp = &(*pp)->Next;
        }
        *pp = b->Next;
        b->InUse = 0;
    }

    b->Base = Base;
    b->Length = V->VolumeBytes - Base < BLOCK_SIZE ? (EXU32)(V->VolumeBytes - Base) : BLOCK_SIZE;
    b->DirtyFrom = b->DirtyTo = 0;
    if (!V->H->Read(V->H->Context, Base, b->Data, b->Length)) {
        Say(V, "Reading the volume at byte %q failed.", Base);
        V->Failed = 1;
        return NULL;
    }
    b->InUse = 1;
    b->Age = ++V->Clock;
    b->Next = V->Hash[Bucket];
    V->Hash[Bucket] = b;
    return b;
}

static int ReadBytes(VOL *V, EXU64 Offset, void *Buffer, EXU32 Length)
{
    EXU8 *To = (EXU8 *)Buffer;

    while (Length != 0) {
        BLK *b = GetBlock(V, Offset);
        EXU32 In, n;
        if (b == NULL) {
            Zero(To, Length);
            return 0;
        }
        In = (EXU32)(Offset - b->Base);
        n = b->Length - In < Length ? b->Length - In : Length;
        Copy(To, b->Data + In, n);
        To += n;
        Offset += n;
        Length -= n;
    }
    return 1;
}

static int WriteBytes(VOL *V, EXU64 Offset, const void *Buffer, EXU32 Length)
{
    const EXU8 *From = (const EXU8 *)Buffer;

    if (!V->Fix) {
        return 0;
    }

    while (Length != 0) {
        BLK *b = GetBlock(V, Offset);
        EXU32 In, n;
        if (b == NULL) {
            return 0;
        }
        In = (EXU32)(Offset - b->Base);
        n = b->Length - In < Length ? b->Length - In : Length;
        Copy(b->Data + In, From, n);
        if (b->DirtyTo <= b->DirtyFrom) {
            b->DirtyFrom = In;
            b->DirtyTo = In + n;
        } else {
            if (In < b->DirtyFrom) b->DirtyFrom = In;
            if (In + n > b->DirtyTo) b->DirtyTo = In + n;
        }
        From += n;
        Offset += n;
        Length -= n;
    }
    return 1;
}

static EXU32 Read32(VOL *V, EXU64 Offset)
{
    EXU8 b[4];
    ReadBytes(V, Offset, b, 4);
    return Get32(b);
}

static void Write32(VOL *V, EXU64 Offset, EXU32 Value)
{
    EXU8 b[4];
    Put32(b, Value);
    WriteBytes(V, Offset, b, 4);
}

/* ------------------------------------------------------------------ */
/* Clusters                                                            */
/* ------------------------------------------------------------------ */

static int Valid(VOL *V, EXU32 c) { return c >= 2 && c <= V->ClusterCount + 1; }
static int IsUsed(VOL *V, EXU32 c) { return (V->Used[(c - 2) >> 3] >> ((c - 2) & 7)) & 1; }
static void MarkUsed(VOL *V, EXU32 c) { V->Used[(c - 2) >> 3] |= (EXU8)(1 << ((c - 2) & 7)); }
static int IsHead(VOL *V, EXU32 c) { return V->Heads != NULL && ((V->Heads[(c - 2) >> 3] >> ((c - 2) & 7)) & 1); }
static void MarkHead(VOL *V, EXU32 c) { V->Heads[(c - 2) >> 3] |= (EXU8)(1 << ((c - 2) & 7)); }
static EXU64 ClusterByte(VOL *V, EXU32 c) { return V->HeapByte + ((EXU64)(c - 2) << V->ClusterShift); }
static EXU32 FatGet(VOL *V, EXU32 c) { return Read32(V, V->FatByte + (EXU64)c * 4); }
static void FatSet(VOL *V, EXU32 c, EXU32 v) { Write32(V, V->FatByte + (EXU64)c * 4, v); }

static EXU32 ClustersFor(VOL *V, EXU64 Bytes)
{
    EXU64 n = (Bytes + V->ClusterSize - 1) >> V->ClusterShift;
    return n > (EXU64)V->ClusterCount + 1 ? V->ClusterCount + 1 : (EXU32)n;
}

/*
 * Follows a stream for Need clusters (or to its end mark with ToEnd),
 * marking what it keeps. List, when given, gets the clusters kept and
 * must have room for Need of them (ToEnd: grown here).
 */
static void Walk(VOL *V, EXU32 First, int NoFat, EXU32 Need, int ToEnd, EXU32 **List, EXU32 *ListCap, CHAIN *C)
{
    EXU32 c = First, Next;

    C->Good = 0;
    C->Last = 0;
    C->Problem = CH_OK;
    C->At = First;

    if (ToEnd) {
        Need = V->ClusterCount + 1;
    }

    while (C->Good < Need && !V->Failed) {

        if (!Valid(V, c)) {
            C->Problem = CH_BAD;
            C->At = c;
            return;
        }
        /* Another stream's first cluster ends this one: whatever follows is not its own */
        if (IsUsed(V, c) || (C->Good != 0 && IsHead(V, c))) {
            C->Problem = CH_CROSS;
            C->At = c;
            return;
        }
        MarkUsed(V, c);
        if (List != NULL) {
            if (ToEnd && !Grow(V, (void **)List, ListCap, C->Good + 1, sizeof(EXU32))) {
                return;
            }
            (*List)[C->Good] = c;
        }
        C->Good++;
        C->Last = c;

        if (NoFat) {
            c++;
            continue;
        }

        Next = FatGet(V, c);
        if (Next == EOC) {
            if (!ToEnd && C->Good < Need) {
                C->Problem = CH_SHORT;
                C->At = c;
            }
            return;
        }
        if (!ToEnd && C->Good == Need) {
            C->Problem = CH_LONG;
            C->At = c;
            return;
        }
        if (Next == BAD_CLUSTER || !Valid(V, Next)) {
            C->Problem = CH_BAD;
            C->At = Next;
            return;
        }
        c = Next;
    }
}

/* The clusters of a stream already walked, into List (room for Count) */
static void ListChain(VOL *V, EXU32 First, int NoFat, EXU32 Count, EXU32 *List)
{
    EXU32 i, c = First;
    for (i = 0; i < Count && !V->Failed; i++) {
        List[i] = c;
        c = NoFat ? c + 1 : FatGet(V, c);
    }
}

/* Takes back the marks Walk left on a stream that is given up */
static void Unmark(VOL *V, EXU32 First, int NoFat, EXU32 Count)
{
    EXU32 i, c = First;
    for (i = 0; i < Count && Valid(V, c) && !V->Failed; i++) {
        V->Used[(c - 2) >> 3] &= (EXU8)~(1 << ((c - 2) & 7));
        c = NoFat ? c + 1 : FatGet(V, c);
    }
}

static const char *ChainText(int Problem)
{
    switch (Problem) {
    case CH_BAD:    return "its cluster chain holds an invalid cluster number";
    case CH_CROSS:  return "it shares clusters with another file (cross-linked)";
    case CH_SHORT:  return "its cluster chain is shorter than its size";
    case CH_LONG:   return "its cluster chain is longer than its size";
    }
    return "";
}

/* ------------------------------------------------------------------ */
/* Checksums and names                                                 */
/* ------------------------------------------------------------------ */

static EXU32 BootChecksum(const EXU8 *Region, EXU32 SectorSize)
{
    EXU32 Sum = 0, i;
    for (i = 0; i < SectorSize * 11; i++) {
        if (i == 106 || i == 107 || i == 112) {
            continue;
        }
        Sum = ((Sum & 1) ? 0x80000000UL : 0) + (Sum >> 1) + Region[i];
    }
    return Sum;
}

static EXU32 SetChecksum(const EXU8 *Set, EXU32 Bytes)
{
    EXU32 Sum = 0, i;
    for (i = 0; i < Bytes; i++) {
        if (i == 2 || i == 3) {
            continue;
        }
        Sum = (((Sum & 1) ? 0x8000U : 0) + (Sum >> 1) + Set[i]) & 0xFFFF;
    }
    return Sum;
}

static EXU32 TableChecksum(const EXU8 *Data, EXU32 Bytes, EXU32 Sum)
{
    EXU32 i;
    for (i = 0; i < Bytes; i++) {
        Sum = ((Sum & 1) ? 0x80000000UL : 0) + (Sum >> 1) + Data[i];
    }
    return Sum;
}

static EXU32 NameHash(VOL *V, const EXU16 *Name, EXU32 Length)
{
    EXU32 Hash = 0, i, c;
    for (i = 0; i < Length; i++) {
        c = V->Upcase[Name[i]];
        Hash = (((Hash & 1) ? 0x8000U : 0) + (Hash >> 1) + (c & 0xFF)) & 0xFFFF;
        Hash = (((Hash & 1) ? 0x8000U : 0) + (Hash >> 1) + (c >> 8)) & 0xFFFF;
    }
    return Hash;
}

static int BadNameChar(EXU32 c)
{
    return c < 0x20 || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' ||
           c == '>' || c == '?' || c == '\\' || c == '|';
}

/* A timestamp as exFAT packs it: 2-second units, minutes, hours, day, month, year */
static int TimeValid(EXU32 t)
{
    EXU32 Day = (t >> 16) & 31, Month = (t >> 21) & 15;
    return (t & 31) <= 29 && ((t >> 5) & 63) <= 59 && ((t >> 11) & 31) <= 23 &&
           Day >= 1 && Month >= 1 && Month <= 12;
}

/* Timestamps (create, modify, access), their 10 ms parts and UTC offsets */
static int FixTimes(EXU8 *Primary, int Fix)
{
    EXU32 i, Good = 0x00210000UL, Bad = 0;
    int Offset;

    for (i = 0; i < 3; i++) {
        if (TimeValid(Get32(Primary + 8 + i * 4))) {
            Good = Get32(Primary + 8 + i * 4);
            break;
        }
    }
    for (i = 0; i < 3; i++) {
        if (!TimeValid(Get32(Primary + 8 + i * 4))) {
            Bad++;
            if (Fix) Put32(Primary + 8 + i * 4, Good);
        }
    }
    for (i = 0; i < 2; i++) {
        if (Primary[20 + i] > 199) {
            Bad++;
            if (Fix) Primary[20 + i] = 0;
        }
    }
    for (i = 0; i < 3; i++) {
        if (Primary[22 + i] & 0x80) {
            Offset = Primary[22 + i] & 0x7F;
            if (Offset & 0x40) {
                Offset -= 0x80;
            }
            if (Offset < -48 || Offset > 56) {
                Bad++;
                if (Fix) Primary[22 + i] = 0;
            }
        }
    }
    return Bad != 0;
}

/* Half of a surrogate pair without its other half */
static int LoneSurrogate(const EXU16 *Name, EXU32 Length, EXU32 i)
{
    EXU32 c = Name[i];
    if (c >= 0xD800 && c <= 0xDBFF) {
        return i + 1 >= Length || Name[i + 1] < 0xDC00 || Name[i + 1] > 0xDFFF;
    }
    if (c >= 0xDC00 && c <= 0xDFFF) {
        return i == 0 || Name[i - 1] < 0xD800 || Name[i - 1] > 0xDBFF;
    }
    return 0;
}

/* Expands a compressed up-case table; 0 if it does not decode */
static int LoadUpcase(VOL *V, const EXU8 *Data, EXU32 Bytes)
{
    EXU32 i, c = 0, n, Count = Bytes / 2;

    for (i = 0; i < 65536; i++) {
        V->Upcase[i] = (EXU16)i;
    }
    for (i = 0; i < Count && c < 65536; i++) {
        EXU32 u = Get16(Data + i * 2);
        if (u == 0xFFFF && i + 1 < Count) {
            n = Get16(Data + ++i * 2);
            c += n;
        } else {
            V->Upcase[c++] = (EXU16)u;
        }
    }
    return c >= 128;
}

static void StandardUpcase(VOL *V)
{
    EXU8 Table[EXFMT_UPCASE_BYTES];
    EXU32 i;
    for (i = 0; i < EXFMT_UPCASE_WORDS; i++) {
        Put16(Table + i * 2, ExfmtUpcase[i]);
    }
    LoadUpcase(V, Table, EXFMT_UPCASE_BYTES);
}

/* ------------------------------------------------------------------ */
/* Names within one directory                                          */
/* ------------------------------------------------------------------ */

static void ResetNames(VOL *V)
{
    EXU32 i;
    for (i = 0; i < NAME_BUCKETS; i++) {
        V->NameBuckets[i] = 0;
    }
    V->NameCount = 0;
    V->ArenaUsed = 0;
}

/* TRUE if an equal name (after up-casing) was added before */
static int NameTaken(VOL *V, const EXU16 *Name, EXU32 Length, EXU32 Hash)
{
    EXU32 r, i;
    for (r = V->NameBuckets[Hash % NAME_BUCKETS]; r != 0; r = V->Names[r - 1].Next) {
        NAMEREC *n = &V->Names[r - 1];
        if (n->Hash != Hash || n->Length != Length) {
            continue;
        }
        for (i = 0; i < Length; i++) {
            if (V->Arena[n->Offset + i] != V->Upcase[Name[i]]) {
                break;
            }
        }
        if (i == Length) {
            return 1;
        }
    }
    return 0;
}

static void AddName(VOL *V, const EXU16 *Name, EXU32 Length, EXU32 Hash)
{
    NAMEREC *n;
    EXU32 i;

    if (!Grow(V, (void **)&V->Names, &V->NameCap, V->NameCount + 1, sizeof(NAMEREC)) ||
        !Grow(V, (void **)&V->Arena, &V->ArenaCap, V->ArenaUsed + Length, sizeof(EXU16))) {
        return;
    }
    n = &V->Names[V->NameCount++];
    n->Hash = Hash;
    n->Length = Length;
    n->Offset = V->ArenaUsed;
    for (i = 0; i < Length; i++) {
        V->Arena[V->ArenaUsed++] = V->Upcase[Name[i]];
    }
    n->Next = V->NameBuckets[Hash % NAME_BUCKETS];
    V->NameBuckets[Hash % NAME_BUCKETS] = V->NameCount;
}

/* ------------------------------------------------------------------ */
/* Directories                                                         */
/* ------------------------------------------------------------------ */

typedef struct _DIR {
    EXU32 *List;
    EXU32 Clusters;
    EXU32 Entries;
    int Root;
} DIR;

static EXU64 EntryByte(VOL *V, DIR *D, EXU32 Index)
{
    EXU32 PerCluster = V->ClusterSize / 32;
    return ClusterByte(V, D->List[Index / PerCluster]) + (EXU64)(Index % PerCluster) * 32;
}

static void ReadEntry(VOL *V, DIR *D, EXU32 Index, EXU8 *e)
{
    ReadBytes(V, EntryByte(V, D, Index), e, 32);
}

static void WriteEntry(VOL *V, DIR *D, EXU32 Index, const EXU8 *e)
{
    WriteBytes(V, EntryByte(V, D, Index), e, 32);
}

/* Marks entries First .. First + Count - 1 unused (in-use bit cleared) */
static void DeleteEntries(VOL *V, DIR *D, EXU32 First, EXU32 Count)
{
    EXU8 e[32];
    EXU32 i;
    for (i = First; i < First + Count && i < D->Entries; i++) {
        ReadEntry(V, D, i, e);
        if (e[0] & 0x80) {
            /* Type 0x80 would become 0x00, the end of the directory */
            e[0] = (EXU8)((e[0] & 0x7F) ? e[0] & 0x7F : 0x05);
            WriteEntry(V, D, i, e);
        }
    }
}

static int Enqueue(VOL *V, EXU32 First, int NoFat, EXU32 Clusters, const EXU16 *Name, EXU32 NameLength)
{
    DIRQ *q;
    EXU32 Length = V->DirPathLength + 1 + NameLength;

    if (Length > MAX_PATH_CHARS) {
        Length = MAX_PATH_CHARS;
    }
    if (!Grow(V, (void **)&V->Queue, &V->QueueCap, V->QueueCount + 1, sizeof(DIRQ))) {
        return 0;
    }
    q = &V->Queue[V->QueueCount];
    q->Path = (EXU16 *)Alloc(V, Length * sizeof(EXU16));
    if (q->Path == NULL) {
        return 0;
    }
    Copy(q->Path, V->DirPath, V->DirPathLength * sizeof(EXU16));
    q->Path[V->DirPathLength] = '\\';
    Copy(q->Path + V->DirPathLength + 1, Name, (Length - V->DirPathLength - 1) * sizeof(EXU16));
    q->PathLength = Length;
    q->First = First;
    q->NoFat = NoFat;
    q->Clusters = Clusters;
    V->QueueCount++;
    return 1;
}

static int SetIntact(VOL *V, DIR *D, EXU32 Index);

/* The start of a stream reads as directory entries */
static int LooksLikeDirectory(VOL *V, EXU32 First, int NoFat, EXU64 DataLength)
{
    EXU32 List[2], Count = ClustersFor(V, DataLength) > 1 ? 2 : 1, i;
    EXU8 e[32];
    DIR D;

    if (!Valid(V, First) || (Count == 2 && !NoFat && !Valid(V, FatGet(V, First)))) {
        return 0;
    }
    ListChain(V, First, NoFat, Count, List);
    D.List = List;
    D.Clusters = Count;
    D.Entries = Count * (V->ClusterSize / 32);
    D.Root = 0;
    ReadEntry(V, &D, 0, e);
    if (e[0] == 0) {
        for (i = 0; i < 32 && e[i] == 0; i++) ;
        return i == 32;
    }
    if (e[0] == 0x05 || e[0] == 0x40 || e[0] == 0x41 || e[0] == 0x20) {
        return 1;
    }
    return e[0] == 0x85 && SetIntact(V, &D, 0);
}

/* A file or directory entry set at Index; returns the entries it takes */
static EXU32 CheckFileSet(VOL *V, DIR *D, EXU32 Index)
{
    EXU8 Set[19 * 32];
    EXU16 Name[256];
    EXU32 Secondary, NameLength, NameEntries, Hash, i, k, Need, Attributes, First, Flags;
    EXU64 DataLength, Vdl;
    int Changed = 0, NameChanged = 0, IsDir, NoFat;
    CHAIN C;
    EXU8 *Stream;

    ReadEntry(V, D, Index, Set);
    Secondary = Set[1];

    if (Secondary < 2 || Secondary > 18 || Index + Secondary >= D->Entries) {
        if (Problem(V, 1, "%p: entry set %u has an invalid secondary count (%u), deleted.", Index, Secondary)) {
            DeleteEntries(V, D, Index, 1);
        }
        return 1;
    }

    for (i = 1; i <= Secondary; i++) {
        ReadEntry(V, D, Index + i, Set + i * 32);
        if (Set[i * 32] == 0) {
            /* The end of the directory comes first: what is before it goes */
            if (Problem(V, 1, "%p: entry set %u is cut short by the end of the directory, deleted.", Index)) {
                DeleteEntries(V, D, Index, i);
            }
            return i;
        }
    }
    Stream = Set + 32;

    /* The layout: stream extension, file names, then benign secondaries */
    NameLength = Stream[3];
    NameEntries = (NameLength + 14) / 15;
    k = Stream[0] == 0xC0 && NameLength != 0 && NameEntries <= Secondary - 1;
    for (i = 0; k && i < NameEntries; i++) {
        k = Set[(2 + i) * 32] == 0xC1;
    }
    if (!k) {
        if (Problem(V, 1, "%p: entry set %u is damaged beyond repair, deleted.", Index)) {
            DeleteEntries(V, D, Index, Secondary + 1);
        }
        return Secondary + 1;
    }

    /* After the names only benign secondaries in use belong to the set */
    for (i = 2 + NameEntries; i <= Secondary && (Set[i * 32] & 0xE0) == 0xE0; i++) ;
    if (i <= Secondary) {
        if (Problem(V, 1, "%p: entry set %u counts entries that are not its own; shortened.", Index)) {
            Set[1] = (EXU8)(i - 1);
            Changed = 1;
        }
        Secondary = i - 1;
    }

    for (i = 0; i < NameLength; i++) {
        Name[i] = (EXU16)Get16(Set + (2 + i / 15) * 32 + 2 + (i % 15) * 2);
    }

    Attributes = Get16(Set + 4);
    IsDir = (Attributes & 0x10) != 0;
    Flags = Stream[1];
    NoFat = (Flags & 2) != 0;
    Vdl = Get64(Stream + 8);
    First = Get32(Stream + 20);
    DataLength = Get64(Stream + 24);

    if (V->P->Verbose) {
        Say(V, "%p\\%n", Name, NameLength);
    }

    if (Get16(Set + 2) != SetChecksum(Set, (Secondary + 1) * 32)) {
        if (Problem(V, 1, "%p\\%n: the entry set checksum is wrong.", Name, NameLength)) {
            Changed = 1;
        }
    }

    for (i = 0; i < NameLength; i++) {
        if (BadNameChar(Name[i]) || LoneSurrogate(Name, NameLength, i)) {
            break;
        }
    }
    if (i < NameLength) {
        if (Problem(V, 1, "%p\\%n: the name holds characters not allowed, replaced by '_'.", Name, NameLength)) {
            for (i = 0; i < NameLength; i++) {
                if (BadNameChar(Name[i]) || LoneSurrogate(Name, NameLength, i)) {
                    Name[i] = '_';
                }
            }
            NameChanged = Changed = 1;
        }
    }

    /* What follows the name in its last entry must be zeros */
    for (i = NameLength; i < NameEntries * 15; i++) {
        if (Get16(Set + (2 + i / 15) * 32 + 2 + (i % 15) * 2) != 0) {
            break;
        }
    }
    if (i < NameEntries * 15) {
        if (Problem(V, 1, "%p\\%n: characters follow the end of the name.", Name, NameLength)) {
            for (i = NameLength; i < NameEntries * 15; i++) {
                Put16(Set + (2 + i / 15) * 32 + 2 + (i % 15) * 2, 0);
            }
            Changed = 1;
        }
    }

    if (FixTimes(Set, 0)) {
        if (Problem(V, 1, "%p\\%n: a timestamp is invalid.", Name, NameLength)) {
            FixTimes(Set, 1);
            Changed = 1;
        }
    }

    Hash = NameHash(V, Name, NameLength);
    if (Get16(Stream + 4) != Hash && !NameChanged) {
        if (Problem(V, 1, "%p\\%n: the name hash is wrong.", Name, NameLength)) {
            Changed = 1;
        }
    }

    if (NameTaken(V, Name, NameLength, Hash)) {
        EXU32 Try, Pos = NameLength - 1;
        if (Problem(V, 1, "%p\\%n: another file has the same name, renamed.", Name, NameLength)) {
            if (Name[Pos] >= 0xD800 && Name[Pos] <= 0xDFFF && Pos > 0) {
                Name[Pos - 1] = '~';
            }
            for (Try = 0; Try < 36; Try++) {
                Name[Pos] = (EXU16)(Try < 10 ? '0' + Try : 'A' + Try - 10);
                if (!NameTaken(V, Name, NameLength, NameHash(V, Name, NameLength))) {
                    break;
                }
            }
            Hash = NameHash(V, Name, NameLength);
            NameChanged = Changed = 1;
        }
    }
    AddName(V, Name, NameLength, Hash);

    if (!(Flags & 1)) {
        if (Problem(V, 1, "%p\\%n: AllocationPossible is clear.", Name, NameLength)) {
            Flags |= 1;
            Changed = 1;
        }
    }

    if (Flags & ~3UL) {
        if (Problem(V, 1, "%p\\%n: unknown stream flags are set.", Name, NameLength)) {
            Flags &= 3;
            Changed = 1;
        }
    }

    for (i = 0; i < NameEntries; i++) {
        if (Set[(2 + i) * 32 + 1] != 0) {
            break;
        }
    }
    if (i < NameEntries) {
        if (Problem(V, 1, "%p\\%n: a file name entry has flags set.", Name, NameLength)) {
            for (i = 0; i < NameEntries; i++) {
                Set[(2 + i) * 32 + 1] = 0;
            }
            Changed = 1;
        }
    }

    if (Vdl > DataLength) {
        if (Problem(V, 1, "%p\\%n: the valid data length is past the end of the file.", Name, NameLength)) {
            Vdl = DataLength;
            Changed = 1;
        }
    }

    if (First == 0 && DataLength != 0) {
        if (Problem(V, 1, "%p\\%n: has a size but no clusters, truncated to 0.", Name, NameLength)) {
            DataLength = Vdl = 0;
            Changed = 1;
        }
    } else if (First != 0 && DataLength == 0 && !IsDir) {
        if (Problem(V, 1, "%p\\%n: has clusters but a size of 0, the clusters are freed.", Name, NameLength)) {
            First = 0;
            Flags &= ~2UL;
            NoFat = 0;
            Changed = 1;
        }
    }

    if (First == 0 && (Flags & 2)) {
        if (Problem(V, 1, "%p\\%n: NoFatChain is set without clusters.", Name, NameLength)) {
            Flags &= ~2UL;
            NoFat = 0;
            Changed = 1;
        }
    }

    /*
     * A directory has clusters, a size of whole clusters and entries at
     * its start. Without that it is taken for a file whose directory
     * attribute is wrong (clearing it loses nothing).
     */
    if (IsDir && (First == 0 || DataLength == 0 ||
                  ((DataLength & (V->ClusterSize - 1)) != 0 && !LooksLikeDirectory(V, First, NoFat, DataLength)))) {
        if (Problem(V, 1, "%p\\%n: marked as a directory but it is not one; made a file.", Name, NameLength)) {
            Attributes &= ~0x10UL;
            IsDir = 0;
            if (DataLength == 0) {
                First = 0;
                Vdl = 0;
                Flags &= ~2UL;
                NoFat = 0;
            }
            Changed = 1;
        }
        if (IsDir) {
            return Secondary + 1;
        }
    }

    Need = 0;
    if (First != 0 && DataLength != 0) {

        Need = ClustersFor(V, DataLength);
        Walk(V, First, NoFat, Need, 0, NULL, NULL, &C);

        if (C.Problem != CH_OK) {

            if (C.Good == 0) {
                if (IsDir) {
                    if (Problem(V, 1, "%p\\%n: %s; the directory is deleted.", Name, NameLength, ChainText(C.Problem))) {
                        DeleteEntries(V, D, Index, Secondary + 1);
                    }
                    return Secondary + 1;
                }
                if (Problem(V, 1, "%p\\%n: %s; truncated to 0.", Name, NameLength, ChainText(C.Problem))) {
                    First = 0;
                    DataLength = Vdl = 0;
                    Flags &= ~2UL;
                    NoFat = 0;
                    Changed = 1;
                }
                Need = 0;

            } else if (C.Problem == CH_LONG) {

                if (Problem(V, 1, "%p\\%n: %s.", Name, NameLength, ChainText(C.Problem))) {
                    FatSet(V, C.Last, EOC);
                }

            } else {

                if (Problem(V, 1, "%p\\%n: %s; truncated to %q bytes.", Name, NameLength,
                            ChainText(C.Problem), (EXU64)C.Good << V->ClusterShift)) {
                    if (!NoFat) {
                        FatSet(V, C.Last, EOC);
                    }
                    if (DataLength > ((EXU64)C.Good << V->ClusterShift)) {
                        DataLength = (EXU64)C.Good << V->ClusterShift;
                    }
                    if (Vdl > DataLength) {
                        Vdl = DataLength;
                    }
                    Changed = 1;
                }
                Need = C.Good;
            }
        }
    }

    if (IsDir && Need != 0) {
        EXU64 Size = (EXU64)Need << V->ClusterShift;
        if (DataLength != Size || Vdl != Size) {
            if (Problem(V, 1, "%p\\%n: the directory size is not a whole number of clusters.", Name, NameLength)) {
                DataLength = Vdl = Size;
                Changed = 1;
            }
        }
    }

    if (Changed && V->Fix) {
        Put16(Set + 4, Attributes);
        Stream[1] = (EXU8)Flags;
        Put16(Stream + 4, Hash);
        Put64(Stream + 8, Vdl);
        Put32(Stream + 20, First);
        Put64(Stream + 24, DataLength);
        if (NameChanged) {
            for (i = 0; i < NameLength; i++) {
                Put16(Set + (2 + i / 15) * 32 + 2 + (i % 15) * 2, Name[i]);
            }
        }
        Put16(Set + 2, SetChecksum(Set, (Secondary + 1) * 32));
        for (i = 0; i <= Secondary; i++) {
            WriteEntry(V, D, Index + i, Set + i * 32);
        }
    }

    if (IsDir) {
        V->R->Directories++;
        if (Need != 0) {
            Enqueue(V, First, NoFat, Need, Name, NameLength);
        }
    } else {
        V->R->Files++;
        V->R->FileBytes += DataLength;
    }

    return Secondary + 1;
}

/* A file entry set at Index whose layout and checksum are right */
static int SetIntact(VOL *V, DIR *D, EXU32 Index)
{
    EXU8 Set[19 * 32];
    EXU32 Secondary, i;

    ReadEntry(V, D, Index, Set);
    Secondary = Set[1];
    if (Set[0] != 0x85 || Secondary < 2 || Secondary > 18 || Index + Secondary >= D->Entries) {
        return 0;
    }
    for (i = 1; i <= Secondary; i++) {
        ReadEntry(V, D, Index + i, Set + i * 32);
    }
    return Set[32] == 0xC0 && Set[64] == 0xC1 &&
           Get16(Set + 2) == SetChecksum(Set, (Secondary + 1) * 32);
}

/* Every entry of a directory; the root's own entries were seen before */
static void CheckDirectory(VOL *V, DIR *D)
{
    EXU8 e[32];
    EXU32 i = 0, Type, End = 0, k;
    int Ended = 0, AfterEnd = 0;

    ResetNames(V);

    while (i < D->Entries && !V->Failed) {

        ReadEntry(V, D, i, e);
        Type = e[0];

        /*
         * Intact files past the end mark: the mark is what was damaged
         * (a sector of zeros), so the zeros become deleted entries.
         */
        if (Ended && Type == 0x85 && SetIntact(V, D, i)) {
            if (Problem(V, 1, "%p: a damaged end-of-directory mark hid files; they are restored.")) {
                for (k = End; k < i; k++) {
                    ReadEntry(V, D, k, e);
                    if (e[0] == 0) {
                        e[0] = 0x05;
                        WriteEntry(V, D, k, e);
                    }
                }
            }
            Ended = 0;
            AfterEnd = 0;
            continue;
        }

        if (Ended) {
            if (Type != 0) {
                if (!AfterEnd) {
                    AfterEnd = Problem(V, 1, "%p: entries follow the end of the directory, cleared.") ? 1 : 2;
                }
                if (AfterEnd == 1) {
                    e[0] = 0;
                    WriteEntry(V, D, i, e);
                }
            }
            i++;
            continue;
        }

        if (Type == 0) {
            Ended = 1;
            End = i;
            i++;
            continue;
        }

        if (!(Type & 0x80)) {
            i++;
            continue;
        }

        if (Type == 0x85) {
            i += CheckFileSet(V, D, i);
            continue;
        }

        if (D->Root && (Type == 0x81 || Type == 0x82 || Type == 0x83)) {
            i++;
            continue;
        }

        /* The benign primaries kept: the volume GUID, TexFAT padding on TexFAT */
        if ((Type == 0xA0 && D->Root) || (Type == 0xA1 && V->NumberOfFats == 2)) {
            i += 1 + e[1];
            continue;
        }

        if (Type >= 0xC0) {
            if (Problem(V, 1, "%p: a secondary entry (%u) without its file entry, deleted.", i)) {
                DeleteEntries(V, D, i, 1);
            }
            i++;
            continue;
        }

        if (Problem(V, 1, "%p: an unknown entry of type %u, deleted.", Type)) {
            DeleteEntries(V, D, i, 1);
        }
        i++;
    }
}

/* ------------------------------------------------------------------ */
/* Root directory, bitmap and up-case table                            */
/* ------------------------------------------------------------------ */

/*
 * The bitmap and the up-case table are contiguous on every volume that
 * formatters make. If their FAT chain wanders off, the FAT is what was
 * damaged: the chain is made contiguous again, unless the contiguous
 * clusters hold the start of some file (then the chain is taken as it is).
 */
static void WalkSystem(VOL *V, const char *What, EXU32 First, EXU32 Need, EXU32 **List, CHAIN *C)
{
    EXU32 i, Broken = 0;

    for (i = 0; i < Need; i++) {
        EXU32 c = First + i;
        if (!Valid(V, c) || IsUsed(V, c) || (i != 0 && IsHead(V, c))) {
            break;
        }
        if (FatGet(V, c) != (i + 1 == Need ? EOC : c + 1)) {
            Broken = 1;
        }
    }

    if (i < Need || !Broken) {
        Walk(V, First, 0, Need, 0, List, NULL, C);
        return;
    }

    if (Problem(V, 1, "The FAT chain of the %s is broken; made contiguous again.", What)) {
        for (i = 0; i < Need; i++) {
            FatSet(V, First + i, i + 1 == Need ? EOC : First + i + 1);
        }
    }
    for (i = 0; i < Need; i++) {
        MarkUsed(V, First + i);
        (*List)[i] = First + i;
    }
    C->Good = Need;
    C->Last = First + Need - 1;
    C->Problem = CH_OK;
    C->At = First;
}

static void CheckUpcase(VOL *V, DIR *D, EXU32 Index, EXU8 *e)
{
    EXU32 First = Get32(e + 20), Stored = Get32(e + 4), Sum = 0, Need, i, n;
    EXU64 Length = Get64(e + 24), Done = 0;
    EXU32 *List;
    EXU8 *Data;
    CHAIN C;

    Need = ClustersFor(V, Length);
    if (First == 0 || Length == 0 || Length > 65536 * 2 + 4) {
        if (Problem(V, 1, "The up-case table entry is invalid; a standard table is written.")) {
            DeleteEntries(V, D, Index, 1);
            V->NeedUpcase = 1;
        }
        StandardUpcase(V);
        return;
    }

    List = (EXU32 *)Alloc(V, Need * sizeof(EXU32));
    Data = (EXU8 *)Alloc(V, (EXU32)Length);
    if (List == NULL || Data == NULL) {
        Free(V, List);
        Free(V, Data);
        return;
    }

    WalkSystem(V, "up-case table", First, Need, &List, &C);
    if (C.Problem == CH_LONG) {
        if (Problem(V, 1, "The up-case table: %s.", ChainText(C.Problem))) {
            FatSet(V, C.Last, EOC);
        }
    } else if (C.Problem != CH_OK) {
        if (Problem(V, 1, "The up-case table: %s; a standard table is written.", ChainText(C.Problem))) {
            DeleteEntries(V, D, Index, 1);
            Unmark(V, First, 0, C.Good);
            V->NeedUpcase = 1;
        }
        StandardUpcase(V);
        Free(V, List);
        Free(V, Data);
        return;
    }

    /* The table is small: at most the first cluster or two */
    for (Done = 0, i = 0; Done < Length; Done += n, i++) {
        n = (EXU32)(Length - Done > V->ClusterSize ? V->ClusterSize : Length - Done);
        ReadBytes(V, ClusterByte(V, List[i]), Data + (EXU32)Done, n);
    }
    Sum = TableChecksum(Data, (EXU32)Length, 0);

    if (Sum == Stored && LoadUpcase(V, Data, (EXU32)Length)) {
        /* good */
    } else if (Sum == EXFMT_UPCASE_CHECKSUM && Length == EXFMT_UPCASE_BYTES) {
        if (Problem(V, 1, "The up-case table checksum in its directory entry is wrong.")) {
            Put32(e + 4, Sum);
            WriteEntry(V, D, Index, e);
        }
        LoadUpcase(V, Data, (EXU32)Length);
    } else if (Length == EXFMT_UPCASE_BYTES) {
        if (Problem(V, 1, "The up-case table is damaged, rewritten with the standard one.")) {
            for (i = 0; i < EXFMT_UPCASE_WORDS; i++) {
                Put16(Data + i * 2, ExfmtUpcase[i]);
            }
            for (Done = 0, i = 0; Done < Length; Done += n, i++) {
                n = (EXU32)(Length - Done > V->ClusterSize ? V->ClusterSize : Length - Done);
                WriteBytes(V, ClusterByte(V, List[i]), Data + (EXU32)Done, n);
            }
            Put32(e + 4, EXFMT_UPCASE_CHECKSUM);
            WriteEntry(V, D, Index, e);
        }
        StandardUpcase(V);
    } else {
        if (Problem(V, 1, "The up-case table is damaged; a standard table is written.")) {
            DeleteEntries(V, D, Index, 1);
            Unmark(V, First, 0, Need);
            V->NeedUpcase = 1;
        }
        StandardUpcase(V);
    }

    Free(V, List);
    Free(V, Data);
}

static void CheckBitmapEntry(VOL *V, DIR *D, EXU32 Index, EXU8 *e)
{
    EXU32 First = Get32(e + 20), Need;
    EXU64 Length = Get64(e + 24);
    CHAIN C;

    if ((e[1] & 1) != 0 && V->NumberOfFats == 1) {
        if (Problem(V, 1, "The allocation bitmap is flagged as a second one, on a volume with one FAT.")) {
            e[1] &= ~1;
            WriteEntry(V, D, Index, e);
        }
    }

    if ((e[1] & 1) != 0 && V->NumberOfFats == 2) {
        /* The second bitmap of a TexFAT volume: only its clusters are counted */
        Walk(V, First, 0, ClustersFor(V, Length), 0, NULL, NULL, &C);
        return;
    }

    if (V->HaveBitmap) {
        if (Problem(V, 1, "A second allocation bitmap entry, deleted.")) {
            DeleteEntries(V, D, Index, 1);
        }
        return;
    }

    if (First == 0 || Length < ((EXU64)V->ClusterCount + 7) / 8) {
        if (Problem(V, 1, "The allocation bitmap entry is invalid; a new bitmap is written.")) {
            DeleteEntries(V, D, Index, 1);
            V->NeedBitmap = 1;
        }
        return;
    }

    Need = ClustersFor(V, Length);
    V->BitmapList = (EXU32 *)Alloc(V, Need * sizeof(EXU32));
    if (V->BitmapList == NULL) {
        return;
    }

    WalkSystem(V, "allocation bitmap", First, Need, &V->BitmapList, &C);
    if (C.Problem == CH_LONG) {
        if (Problem(V, 1, "The allocation bitmap: %s.", ChainText(C.Problem))) {
            FatSet(V, C.Last, EOC);
        }
    } else if (C.Problem != CH_OK) {
        if (Problem(V, 1, "The allocation bitmap: %s; a new bitmap is written.", ChainText(C.Problem))) {
            DeleteEntries(V, D, Index, 1);
            V->NeedBitmap = 1;
        }
        Unmark(V, First, 0, C.Good);
        Free(V, V->BitmapList);
        V->BitmapList = NULL;
        return;
    }

    V->HaveBitmap = 1;
    V->BitmapClusters = Need;
    V->BitmapLength = Length;
}

/* The root's own entries: bitmap, up-case table, label */
static void CheckRootEntries(VOL *V, DIR *D)
{
    EXU8 e[32];
    EXU32 i, UpcaseIndex = 0;
    int Upcase = 0;
    EXU8 UpcaseEntry[32];

    for (i = 0; i < D->Entries && !V->Failed; i++) {
        ReadEntry(V, D, i, e);
        if (e[0] == 0) {
            break;
        }
        if (e[0] == 0x81) {
            CheckBitmapEntry(V, D, i, e);
        } else if (e[0] == 0x82) {
            if (Upcase) {
                if (Problem(V, 1, "A second up-case table entry, deleted.")) {
                    DeleteEntries(V, D, i, 1);
                }
            } else {
                Upcase = 1;
                UpcaseIndex = i;
                Copy(UpcaseEntry, e, 32);
            }
        } else if (e[0] == 0x83 && e[1] > 11) {
            if (Problem(V, 1, "The volume label is too long, cut to 11 characters.")) {
                e[1] = 11;
                WriteEntry(V, D, i, e);
            }
        }
    }

    if (Upcase) {
        CheckUpcase(V, D, UpcaseIndex, UpcaseEntry);
    } else {
        if (Problem(V, 1, "The up-case table entry is missing; a standard table is written.")) {
            V->NeedUpcase = 1;
        }
        StandardUpcase(V);
    }

    if (!V->HaveBitmap && !V->NeedBitmap && !V->Failed) {
        if (Problem(V, 1, "The allocation bitmap entry is missing; a new bitmap is written.")) {
            V->NeedBitmap = 1;
        }
    }
}

/* The first run of Count free clusters, marked used and chained; 0 if none */
static EXU32 TakeClusters(VOL *V, EXU32 Count)
{
    EXU32 c, Run = 0, First = 0, i;

    for (c = 2; c <= V->ClusterCount + 1; c++) {
        if (IsUsed(V, c)) {
            Run = 0;
            continue;
        }
        if (Run++ == 0) {
            First = c;
        }
        if (Run == Count) {
            for (i = 0; i < Count; i++) {
                MarkUsed(V, First + i);
                FatSet(V, First + i, i + 1 == Count ? EOC : First + i + 1);
            }
            return First;
        }
    }
    return 0;
}

/* A free entry of the root directory (deleted or past the end); 0 if none */
static int RootSlot(VOL *V, EXU32 *Index)
{
    EXU8 e[32];
    EXU32 i, Entries = V->RootClusters * (V->ClusterSize / 32);
    DIR D;

    D.List = V->RootList;
    D.Clusters = V->RootClusters;
    D.Entries = Entries;
    D.Root = 1;
    for (i = 0; i < Entries; i++) {
        ReadEntry(V, &D, i, e);
        if (e[0] == 0 || !(e[0] & 0x80)) {
            *Index = i;
            return 1;
        }
    }
    return 0;
}

static void PutRootEntry(VOL *V, EXU32 Index, const EXU8 *e)
{
    DIR D;
    D.List = V->RootList;
    D.Clusters = V->RootClusters;
    D.Entries = V->RootClusters * (V->ClusterSize / 32);
    D.Root = 1;
    WriteEntry(V, &D, Index, e);
}

/* The up-case table and the allocation bitmap, written again where they were lost */
static void Recreate(VOL *V)
{
    EXU8 e[32], Table[EXFMT_UPCASE_BYTES];
    EXU32 First, Count, Index, i, n, Done;

    if (V->NeedUpcase) {
        Count = ClustersFor(V, EXFMT_UPCASE_BYTES);
        if (!RootSlot(V, &Index) || (First = TakeClusters(V, Count)) == 0) {
            Say(V, "There is no room for a new up-case table.");
            V->R->Fixed--;
        } else {
            for (i = 0; i < EXFMT_UPCASE_WORDS; i++) {
                Put16(Table + i * 2, ExfmtUpcase[i]);
            }
            for (Done = 0, i = 0; Done < EXFMT_UPCASE_BYTES; Done += n, i++) {
                n = EXFMT_UPCASE_BYTES - Done > V->ClusterSize ? V->ClusterSize : EXFMT_UPCASE_BYTES - Done;
                WriteBytes(V, ClusterByte(V, First + i), Table + Done, n);
            }
            Zero(e, 32);
            e[0] = 0x82;
            Put32(e + 4, EXFMT_UPCASE_CHECKSUM);
            Put32(e + 20, First);
            Put64(e + 24, EXFMT_UPCASE_BYTES);
            PutRootEntry(V, Index, e);
        }
    }

    if (V->NeedBitmap) {
        EXU64 Length = ((EXU64)V->ClusterCount + 7) / 8;
        Count = ClustersFor(V, Length);
        V->BitmapList = (EXU32 *)Alloc(V, Count * sizeof(EXU32));
        if (V->BitmapList == NULL || !RootSlot(V, &Index) || (First = TakeClusters(V, Count)) == 0) {
            Say(V, "There is no room for a new allocation bitmap.");
            V->R->Fixed--;
        } else {
            for (i = 0; i < Count; i++) {
                V->BitmapList[i] = First + i;
            }
            Zero(e, 32);
            e[0] = 0x81;
            Put32(e + 20, First);
            Put64(e + 24, Length);
            PutRootEntry(V, Index, e);
            V->HaveBitmap = 1;
            V->FreshBitmap = 1;
            V->BitmapClusters = Count;
            V->BitmapLength = Length;
        }
    }
}

/* ------------------------------------------------------------------ */
/* The allocation bitmap against the clusters in use                   */
/* ------------------------------------------------------------------ */

static void CompareBitmap(VOL *V)
{
    EXU32 Bytes = (V->ClusterCount + 7) / 8;
    EXU32 PerCluster = V->ClusterSize, Offset, n, i, Leaked = 0, Missing = 0;
    EXU8 Buffer[4096];
    int Pass, Fix = 0;

    if (!V->HaveBitmap) {
        return;
    }

    /* First count, then (if asked) write what should be there */
    for (Pass = 0; Pass < 2 && !V->Failed; Pass++) {

        for (Offset = 0; Offset < Bytes; Offset += n) {

            EXU32 In = Offset % PerCluster;
            EXU64 At = ClusterByte(V, V->BitmapList[Offset / PerCluster]) + In;
            int Differs = 0;

            n = PerCluster - In;
            if (n > sizeof(Buffer)) n = sizeof(Buffer);
            if (n > Bytes - Offset) n = Bytes - Offset;

            ReadBytes(V, At, Buffer, n);
            if (V->Failed) {
                return;
            }

            for (i = 0; i < n; i++) {
                EXU8 Want = V->Used[Offset + i], Have = Buffer[i], Mask = 0xFF, Diff;
                EXU32 b;
                if (Offset + i == Bytes - 1 && (V->ClusterCount & 7)) {
                    Mask = (EXU8)((1 << (V->ClusterCount & 7)) - 1);
                }
                Diff = (EXU8)((Want ^ Have) & Mask);
                if (Diff == 0) {
                    continue;
                }
                Differs = 1;
                if (Pass == 0) {
                    for (b = 0; b < 8; b++) {
                        if (Diff & (1 << b)) {
                            if (Have & (1 << b)) Leaked++; else Missing++;
                        }
                    }
                } else {
                    Buffer[i] = (EXU8)((Have & ~Mask) | (Want & Mask));
                }
            }

            if (Pass == 1 && Differs) {
                WriteBytes(V, At, Buffer, n);
            }
        }

        if (Pass == 0 && V->FreshBitmap) {
            Fix = 1;
        } else if (Pass == 0) {
            if (Leaked && Problem(V, 1, "%u clusters are marked in use in the allocation bitmap but used by nothing; freed.", Leaked)) {
                Fix = 1;
            }
            if (Missing && Problem(V, 1, "%u clusters in use are marked free in the allocation bitmap; marked.", Missing)) {
                Fix = 1;
            }
            if (!Fix) {
                return;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Boot region                                                         */
/* ------------------------------------------------------------------ */

/* The fields that place things describe a possible volume */
static int GeometrySane(VOL *V, const EXU8 *B)
{
    EXU64 VolumeLength = Get64(B + 72);
    EXU32 FatOffset = Get32(B + 80), FatLength = Get32(B + 84), HeapOffset = Get32(B + 88);
    EXU32 Count = Get32(B + 92), Root = Get32(B + 96);
    EXU32 SectorShift = B[108], ClusterShift = B[109], Fats = B[110];

    if (SectorShift < 9 || SectorShift > 12 || SectorShift + ClusterShift > 25 ||
        (Fats != 1 && Fats != 2) || B[105] != 1 || FatOffset < 24 ||
        ((EXU64)FatLength << SectorShift) < ((EXU64)Count + 2) * 4 ||
        HeapOffset < FatOffset + FatLength * Fats ||
        Count == 0 || Count > EXFMT_MAX_CLUSTERS ||
        (EXU64)HeapOffset + ((EXU64)Count << ClusterShift) > VolumeLength ||
        Root < 2 || Root > Count + 1) {
        return 0;
    }
    if (V->P->VolumeSectors != 0 && VolumeLength > V->P->VolumeSectors) {
        return 0;
    }
    return 1;
}

/* An exFAT boot sector with a possible layout (the checksum aside) */
static int RegionSane(VOL *V, const EXU8 *R, EXU32 SectorSize)
{
    EXU32 i;

    if (Get16(R + 510) != 0xAA55 || R[0] != 0xEB || R[1] != 0x76 || R[2] != 0x90) {
        return 0;
    }
    for (i = 0; i < 8; i++) {
        if (R[3 + i] != (EXU8)"EXFAT   "[i]) {
            return 0;
        }
    }
    for (i = 11; i < 64; i++) {
        if (R[i] != 0) {
            return 0;
        }
    }
    if ((1UL << R[108]) != SectorSize) {
        return 0;
    }
    return GeometrySane(V, R);
}

/* The checksum sector holds the checksum of the 11 before it */
static int RegionSum(EXU8 *R, EXU32 SectorSize, int Set)
{
    EXU32 i, Sum = BootChecksum(R, SectorSize);

    for (i = 0; i < SectorSize; i += 4) {
        if (Set) {
            Put32(R + 11 * SectorSize + i, Sum);
        } else if (Get32(R + 11 * SectorSize + i) != Sum) {
            return 0;
        }
    }
    return 1;
}

/*
 * How well a boot sector's layout fits the disk: the FAT starts with its
 * two fixed entries, the root directory holds the bitmap and up-case
 * table entries. 0 to 3.
 */
static int Probe(VOL *V, const EXU8 *B)
{
    EXU32 Shift = B[108], Size = 1UL << Shift, i, Score = 0, Found = 0;
    EXU64 Root;
    EXU8 *Sector;

    if (Size != V->SectorSize || !GeometrySane(V, B)) {
        return 0;
    }
    Sector = (EXU8 *)Alloc(V, Size);
    if (Sector == NULL) {
        return 0;
    }
    if (V->H->Read(V->H->Context, (EXU64)Get32(B + 80) << Shift, Sector, Size) &&
        Get32(Sector) == 0xFFFFFFF8UL && Get32(Sector + 4) == EOC) {
        Score++;
    }
    Root = ((EXU64)Get32(B + 88) << Shift) + ((EXU64)(Get32(B + 96) - 2) << (Shift + B[109]));
    if (V->H->Read(V->H->Context, Root, Sector, Size)) {
        for (i = 0; i < Size; i += 32) {
            if (Sector[i] == 0x81) Found |= 1;
            if (Sector[i] == 0x82) Found |= 2;
        }
    }
    Score += (Found & 1) + ((Found >> 1) & 1);
    Free(V, Sector);
    return (int)Score;
}

/* The value most of the words of a checksum sector hold */
static EXU32 MajorityWord(const EXU8 *Sector, EXU32 Size)
{
    EXU32 i, j, Best = 0, BestCount = 0, Count;

    for (i = 0; i < Size && BestCount * 2 <= Size / 4; i += 4) {
        for (Count = 0, j = 0; j < Size; j += 4) {
            Count += Get32(Sector + j) == Get32(Sector + i);
        }
        if (Count > BestCount) {
            BestCount = Count;
            Best = Get32(Sector + i);
        }
    }
    return Best;
}

/*
 * Two damaged copies of the boot region, damaged in different places:
 * each byte where they differ is taken from one or the other until the
 * checksum either copy stored comes out. The search goes depth first, so
 * the checksum up to a differing byte is computed once for both choices.
 */
#define COMBINE_MAX 16

typedef struct _COMBINE {
    VOL *V;
    EXU8 *Main;
    const EXU8 *Backup;
    EXU32 Size;
    EXU32 Pos[COMBINE_MAX];
    EXU8 Old[COMBINE_MAX];
    EXU32 Count;
    EXU32 Want1;
    EXU32 Want2;
} COMBINE;

static EXU32 SumBytes(const EXU8 *R, EXU32 From, EXU32 To, EXU32 Sum)
{
    EXU32 i;
    for (i = From; i < To; i++) {
        if (i != 106 && i != 107 && i != 112) {
            Sum = ((Sum & 1) ? 0x80000000UL : 0) + (Sum >> 1) + R[i];
        }
    }
    return Sum;
}

static int Search(COMBINE *c, EXU32 k, EXU32 Sum, EXU32 From)
{
    EXU32 To = k < c->Count ? c->Pos[k] : 11 * c->Size;
    int Choice;

    Sum = SumBytes(c->Main, From, To, Sum);
    if (k == c->Count) {
        return (Sum == c->Want1 || Sum == c->Want2) && RegionSane(c->V, c->Main, c->Size);
    }
    for (Choice = 0; Choice < 2; Choice++) {
        c->Main[To] = Choice ? c->Backup[To] : c->Old[k];
        if (Search(c, k + 1, SumBytes(c->Main, To, To + 1, Sum), To + 1)) {
            return 1;
        }
    }
    c->Main[To] = c->Old[k];
    return 0;
}

static int Combine(VOL *V, EXU8 *Main, const EXU8 *Backup, EXU32 Size)
{
    COMBINE c;
    EXU32 i;

    c.V = V;
    c.Main = Main;
    c.Backup = Backup;
    c.Size = Size;
    c.Count = 0;
    for (i = 0; i < 11 * Size; i++) {
        if (i != 106 && i != 107 && i != 112 && Main[i] != Backup[i]) {
            if (c.Count == COMBINE_MAX) {
                return 0;
            }
            c.Pos[c.Count] = i;
            c.Old[c.Count++] = Main[i];
        }
    }
    if (c.Count == 0) {
        return 0;
    }
    c.Want1 = MajorityWord(Main + 11 * Size, Size);
    c.Want2 = MajorityWord(Backup + 11 * Size, Size);

    if (!Search(&c, 0, 0, 0)) {
        return 0;
    }
    RegionSum(Main, Size, 1);
    return 1;
}

/* A boot region made whole again around its layout */
static void Rebuild(EXU8 *R, EXU32 Size)
{
    EXU32 i;
    R[0] = 0xEB;
    R[1] = 0x76;
    R[2] = 0x90;
    for (i = 0; i < 8; i++) {
        R[3 + i] = (EXU8)"EXFAT   "[i];
    }
    for (i = 11; i < 64; i++) {
        R[i] = 0;
    }
    Put16(R + 510, 0xAA55);
    RegionSum(R, Size, 1);
}

static void TakeGeometry(VOL *V, const EXU8 *B)
{
    EXU32 SectorShift = B[108];

    V->SectorShift = SectorShift;
    V->SectorSize = 1UL << SectorShift;
    V->ClusterShift = SectorShift + B[109];
    V->ClusterSize = 1UL << V->ClusterShift;
    V->VolumeBytes = Get64(B + 72) << SectorShift;
    V->FatByte = (EXU64)Get32(B + 80) << SectorShift;
    V->FatLength = Get32(B + 84);
    V->HeapByte = (EXU64)Get32(B + 88) << SectorShift;
    V->ClusterCount = Get32(B + 92);
    V->RootCluster = Get32(B + 96);
    V->NumberOfFats = B[110];
    V->VolumeFlags = Get16(B + 106);
    V->PercentInUse = B[112];

    /* With two FATs the active one is chosen by the flags */
    if (V->NumberOfFats == 2 && (V->VolumeFlags & 1)) {
        V->FatByte += (EXU64)V->FatLength << SectorShift;
    }
}

static int WriteRegion(VOL *V, EXU32 Sector, const EXU8 *Region)
{
    if (!V->H->Write(V->H->Context, (EXU64)Sector * V->SectorSize, Region, 12 * V->SectorSize)) {
        Say(V, "Writing the boot region failed.");
        V->Failed = 1;
        return 0;
    }
    return 1;
}

/* Main and backup boot regions; 0 if neither can be used */
static int CheckBootRegion(VOL *V)
{
    EXU32 Size = V->SectorSize, i;
    EXU8 *Main, *Backup;
    int MainSane, MainSum, BackupSane, BackupSum, Same = 1, Result = 1;

    Main = (EXU8 *)Alloc(V, 24 * Size);
    if (Main == NULL) {
        return 0;
    }
    Backup = Main + 12 * Size;

    if (!V->H->Read(V->H->Context, 0, Main, 24 * Size)) {
        Say(V, "Reading the boot region failed.");
        V->Failed = 1;
        Free(V, Main);
        return 0;
    }

    MainSane = RegionSane(V, Main, Size);
    MainSum = MainSane && RegionSum(Main, Size, 0);
    BackupSane = RegionSane(V, Backup, Size);
    BackupSum = BackupSane && RegionSum(Backup, Size, 0);

    for (i = 0; i < 11 * Size && Same; i++) {
        if (i != 106 && i != 107 && i != 112 && Main[i] != Backup[i]) {
            Same = 0;
        }
    }

    if (MainSum) {

        TakeGeometry(V, Main);
        if (!BackupSum || !Same) {
            if (Problem(V, 1, "The backup boot region is damaged or differs, rewritten from the main one.")) {
                WriteRegion(V, 12, Main);
            }
        }

    } else if (BackupSum) {

        TakeGeometry(V, Backup);
        if (Problem(V, 1, "The main boot region is damaged, restored from the backup.")) {
            WriteRegion(V, 0, Backup);
        } else {
            /* Checking goes on from the backup's view, but nothing is fixed */
            V->Fix = 0;
        }

    } else {

        /*
         * Both are damaged. The layout that fits the disk (FAT and root
         * directory where it says) is kept, the rest of the region made
         * whole again around it.
         */
        int MainScore, BackupScore;
        EXU8 *Chosen = NULL;

        if (Combine(V, Main, Backup, Size)) {
            TakeGeometry(V, Main);
            if (Problem(V, 1, "Both boot regions are damaged; the original is put together from the two.")) {
                if (WriteRegion(V, 12, Main)) {
                    WriteRegion(V, 0, Main);
                }
            } else {
                V->Fix = 0;
            }
            Free(V, Main);
            return !V->Failed;
        }

        MainScore = Probe(V, Main);
        BackupScore = Probe(V, Backup);

        /* The layout: all but the serial number and the flags */
        for (i = 64; i < 112; i++) {
            if ((i < 100 || i > 103) && i != 106 && i != 107 && Main[i] != Backup[i]) {
                break;
            }
        }
        if (MainScore >= 2 && (MainScore > BackupScore || (i == 112 && MainScore == BackupScore))) {
            Chosen = Main;
        } else if (BackupScore >= 2 && BackupScore > MainScore) {
            Chosen = Backup;
        }

        if (Chosen == NULL) {
            Say(V, (MainSane || BackupSane || MainScore || BackupScore) ?
                "Both boot regions are damaged, and neither layout fits the volume." :
                "Neither the boot region nor its backup is a valid exFAT boot region.");
            Result = 0;
        } else {
            TakeGeometry(V, Chosen);
            if (Problem(V, 1, "Both boot regions are damaged; rebuilt from the %s one.",
                        Chosen == Main ? "main" : "backup")) {
                Rebuild(Chosen, Size);
                if (WriteRegion(V, 12, Chosen)) {
                    WriteRegion(V, 0, Chosen);
                }
            } else {
                V->Fix = 0;
            }
        }
    }

    Free(V, Main);
    return Result && !V->Failed;
}

/*
 * A first pass over the directories, changing nothing: the first cluster
 * of every intact entry set. A stream that runs into one of them (a
 * contiguous run made too long, a chain through stale FAT entries) is
 * cut there, and does not take the other file's clusters.
 */
typedef struct _HEADQ {
    EXU32 First;
    EXU32 Clusters;
    int NoFat;
} HEADQ;

static void CollectHeads(VOL *V)
{
    EXU32 Bytes = (V->ClusterCount + 7) / 8 + 1, Head, Cap = 0, Count = 0, n, i, c, *List = NULL, ListCap = 0;
    EXU8 *Seen, e[64];
    HEADQ *Q = NULL;
    DIR D;

    V->Heads = (EXU8 *)Alloc(V, Bytes);
    Seen = (EXU8 *)Alloc(V, Bytes);
    if (V->Heads == NULL || Seen == NULL || !Grow(V, (void **)&Q, &Cap, 1, sizeof(HEADQ))) {
        Free(V, V->Heads);
        V->Heads = NULL;
        Free(V, Seen);
        return;
    }

    Q[0].First = V->RootCluster;
    Q[0].Clusters = V->ClusterCount;
    Q[0].NoFat = 0;
    Count = 1;

    for (Head = 0; Head < Count && !V->Failed; Head++) {

        /* The directory's clusters, each only once in the whole pass */
        c = Q[Head].First;
        for (n = 0; n < Q[Head].Clusters && Valid(V, c); n++) {
            if ((Seen[(c - 2) >> 3] >> ((c - 2) & 7)) & 1) {
                break;
            }
            Seen[(c - 2) >> 3] |= (EXU8)(1 << ((c - 2) & 7));
            if (!Grow(V, (void **)&List, &ListCap, n + 1, sizeof(EXU32))) {
                break;
            }
            List[n] = c;
            c = Q[Head].NoFat ? c + 1 : FatGet(V, c);
            if (!Q[Head].NoFat && c == EOC) {
                n++;
                break;
            }
        }

        D.List = List;
        D.Clusters = n;
        D.Entries = n * (V->ClusterSize / 32);
        D.Root = Head == 0;

        for (i = 0; i < D.Entries && !V->Failed; i++) {
            ReadEntry(V, &D, i, e);
            if (e[0] == 0) {
                break;
            }
            if ((e[0] == 0x81 || e[0] == 0x82) && D.Root && Valid(V, Get32(e + 20))) {
                MarkHead(V, Get32(e + 20));
                continue;
            }
            if (e[0] != 0x85 || !SetIntact(V, &D, i)) {
                continue;
            }
            ReadEntry(V, &D, i + 1, e + 32);
            c = Get32(e + 32 + 20);
            if (Valid(V, c) && Get64(e + 32 + 24) != 0) {
                MarkHead(V, c);
                if ((Get16(e + 4) & 0x10) && Grow(V, (void **)&Q, &Cap, Count + 1, sizeof(HEADQ))) {
                    Q[Count].First = c;
                    Q[Count].Clusters = ClustersFor(V, Get64(e + 32 + 24));
                    Q[Count].NoFat = (e[32 + 1] & 2) != 0;
                    Count++;
                }
            }
            i += e[1];
        }
    }

    Free(V, List);
    Free(V, Q);
    Free(V, Seen);
}

/* ------------------------------------------------------------------ */
/* The whole volume                                                    */
/* ------------------------------------------------------------------ */

int ExcQuickState(const EXU8 *Boot)
{
    EXU32 i;
    if (Get16(Boot + 510) != 0xAA55) {
        return EXC_NOT_EXFAT;
    }
    for (i = 0; i < 8; i++) {
        if (Boot[3 + i] != (EXU8)"EXFAT   "[i]) {
            return EXC_NOT_EXFAT;
        }
    }
    return (Get16(Boot + 106) & 2) ? EXC_STATE_DIRTY : EXC_STATE_CLEAN;
}

static void Release(VOL *V)
{
    EXU32 i;
    for (i = 0; i < BLOCK_COUNT; i++) {
        Free(V, V->Blocks[i].Data);
    }
    for (i = 0; i < V->QueueCount; i++) {
        Free(V, V->Queue[i].Path);
    }
    Free(V, V->Queue);
    Free(V, V->Used);
    Free(V, V->Heads);
    Free(V, V->Upcase);
    Free(V, V->BitmapList);
    Free(V, V->RootList);
    Free(V, V->Names);
    Free(V, V->Arena);
}

int ExcCheck(const EXC_PARAMS *Params, const EXC_HOST *Host, EXC_RESULT *Result)
{
    VOL *V;
    DIR D;
    EXU32 i, *RootList = NULL, RootCap = 0, Used, Percent, Head;
    CHAIN C;
    EXU8 Sector0[4096];
    int Status;
    int (*HostRead)(void *, EXU64, void *, EXU32) = Host->Read;

    Zero(Result, sizeof(*Result));

    V = (VOL *)Host->Alloc(Host->Context, sizeof(VOL));
    if (V == NULL) {
        return EXC_FAILED;
    }
    Zero(V, sizeof(*V));
    V->P = Params;
    V->H = Host;
    V->R = Result;
    V->Fix = Params->Fix;

    /* The sector size, from the device or from the boot sector */
    V->SectorSize = Params->SectorSize;
    if (V->SectorSize == 0) {
        if (!HostRead(Host->Context, 0, Sector0, 4096)) {
            Say(V, "Reading the boot sector failed.");
            Host->Free(Host->Context, V);
            return EXC_FAILED;
        }
        V->SectorSize = (Sector0[108] >= 9 && Sector0[108] <= 12) ? 1UL << Sector0[108] : 512;
    }

    if (!CheckBootRegion(V)) {
        Status = EXC_FAILED;
        goto Done;
    }

    Result->ClusterSize = V->ClusterSize;
    Result->ClusterCount = V->ClusterCount;
    Result->WasDirty = (V->VolumeFlags & 2) != 0;

    if (V->NumberOfFats != 1 && V->Fix) {
        Say(V, "This is a TexFAT volume (two FATs): it is only checked, not repaired.");
        V->Fix = 0;
    }

    for (i = 0; i < BLOCK_COUNT; i++) {
        V->Blocks[i].Data = (EXU8 *)Alloc(V, BLOCK_SIZE);
    }
    V->Used = (EXU8 *)Alloc(V, (V->ClusterCount + 7) / 8 + 1);
    V->Upcase = (EXU16 *)Alloc(V, 65536 * sizeof(EXU16));
    if (V->Failed) {
        Say(V, "Not enough memory.");
        Status = EXC_FAILED;
        goto Done;
    }
    StandardUpcase(V);

    CollectHeads(V);

    /* FAT entries 0 and 1 */
    if (FatGet(V, 0) != 0xFFFFFFF8UL || FatGet(V, 1) != EOC) {
        if (Problem(V, 1, "The first two FAT entries are wrong.")) {
            FatSet(V, 0, 0xFFFFFFF8UL);
            FatSet(V, 1, EOC);
        }
    }

    /* The root directory: its chain runs to the end mark */
    Walk(V, V->RootCluster, 0, 0, 1, &RootList, &RootCap, &C);
    V->RootList = RootList;
    if (V->Failed) {
        Status = EXC_FAILED;
        goto Done;
    }
    if (C.Problem != CH_OK) {
        if (C.Good == 0) {
            Say(V, "The root directory's first cluster is invalid.");
            Status = EXC_FAILED;
            goto Done;
        }
        if (Problem(V, 1, "The root directory: %s; it ends after %u clusters.", ChainText(C.Problem), C.Good)) {
            FatSet(V, C.Last, EOC);
        }
    }

    D.List = RootList;
    D.Clusters = C.Good;
    D.Entries = C.Good * (V->ClusterSize / 32);
    D.Root = 1;
    V->DirPath = NULL;
    V->DirPathLength = 0;

    V->RootClusters = C.Good;

    CheckRootEntries(V, &D);
    CheckDirectory(V, &D);
    Result->Directories++;

    /* The other directories, breadth first */
    for (Head = 0; Head < V->QueueCount && !V->Failed; Head++) {
        DIRQ *q = &V->Queue[Head];
        D.List = (EXU32 *)Alloc(V, q->Clusters * sizeof(EXU32));
        if (D.List == NULL) {
            break;
        }
        ListChain(V, q->First, q->NoFat, q->Clusters, D.List);
        D.Clusters = q->Clusters;
        D.Entries = q->Clusters * (V->ClusterSize / 32);
        D.Root = 0;
        V->DirPath = q->Path;
        V->DirPathLength = q->PathLength;
        CheckDirectory(V, &D);
        Free(V, D.List);
        /* The queue may have moved: take the path through the index */
        Free(V, V->Queue[Head].Path);
        V->Queue[Head].Path = NULL;
    }
    V->DirPath = NULL;
    V->DirPathLength = 0;

    if (V->Failed) {
        Status = EXC_FAILED;
        goto Done;
    }

    if (V->Fix) {
        Recreate(V);
    }
    CompareBitmap(V);

    for (Used = 0, i = 0; i < (V->ClusterCount + 7) / 8; i++) {
        EXU32 b = V->Used[i];
        while (b) {
            Used += b & 1;
            b >>= 1;
        }
    }
    Result->UsedClusters = Used;
    Percent = (EXU32)((EXU64)Used * 100 / V->ClusterCount);

    if (!(V->VolumeFlags & 2) && V->PercentInUse != 0xFF &&
        (V->PercentInUse > Percent + 1 || V->PercentInUse + 1 < Percent)) {
        Problem(V, 1, "The percentage in use in the boot sector (%u) is wrong (%u).", V->PercentInUse, Percent);
    }

    if (V->VolumeFlags & 2) {
        Problem(V, Result->Problems == Result->Fixed, "The volume is marked dirty.");
    }

    if (!FlushAll(V)) {
        Status = EXC_FAILED;
        goto Done;
    }

    /* Everything is on the disk: the volume flags go last */
    if (V->Fix && Result->Problems == Result->Fixed && Result->Problems != 0) {
        EXU8 *Boot = (EXU8 *)Alloc(V, V->SectorSize);
        if (Boot != NULL && HostRead(Host->Context, 0, Boot, V->SectorSize)) {
            Put16(Boot + 106, V->VolumeFlags & ~2UL);
            Boot[112] = (EXU8)Percent;
            if (!Host->Write(Host->Context, 0, Boot, V->SectorSize)) {
                V->Failed = 1;
            }
        } else {
            V->Failed = 1;
        }
        Free(V, Boot);
    }

    if (V->Failed) {
        Status = EXC_FAILED;
    } else if (Result->Problems == 0) {
        Status = EXC_CLEAN;
    } else if (Result->Problems == Result->Fixed) {
        Status = EXC_FIXED;
    } else {
        Status = EXC_ERRORS;
    }

Done:
    Release(V);
    Host->Free(Host->Context, V);
    return Status;
}
