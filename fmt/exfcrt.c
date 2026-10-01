/*
 * EXFCRT - the few C library routines the tools use, on kernel32 only
 * (see exfcrt.h). The entry point is ExfCrtStart: link with
 * -entry:ExfCrtStart -nodefaultlib, and libc.lib for the 64-bit helpers.
 */

#include <windows.h>
#include "exfcrt.h"

#ifdef _MSC_VER
#pragma function(memcpy, memset, memcmp, strlen, strcmp, strcpy, strcat)
#endif

int main(int argc, char **argv);

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

void *calloc(exf_size_t Count, exf_size_t Size)
{
    if (Size != 0 && Count > 0xFFFFFFFFUL / Size) {
        return NULL;
    }
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Count * Size);
}

void *malloc(exf_size_t Size)
{
    return HeapAlloc(GetProcessHeap(), 0, Size);
}

void free(void *Block)
{
    if (Block != NULL) {
        HeapFree(GetProcessHeap(), 0, Block);
    }
}

void *memcpy(void *To, const void *From, exf_size_t Length)
{
    unsigned char *t = (unsigned char *)To;
    const unsigned char *f = (const unsigned char *)From;
    while (Length--) *t++ = *f++;
    return To;
}

void *memset(void *To, int Value, exf_size_t Length)
{
    unsigned char *t = (unsigned char *)To;
    while (Length--) *t++ = (unsigned char)Value;
    return To;
}

int memcmp(const void *A, const void *B, exf_size_t Length)
{
    const unsigned char *a = (const unsigned char *)A;
    const unsigned char *b = (const unsigned char *)B;
    for (; Length != 0; Length--, a++, b++) {
        if (*a != *b) return *a < *b ? -1 : 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Characters and strings (ASCII is all the tools need)               */
/* ------------------------------------------------------------------ */

int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c; }
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }

exf_size_t strlen(const char *s)
{
    exf_size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int _stricmp(const char *a, const char *b)
{
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

char *strcpy(char *t, const char *f)
{
    char *r = t;
    while ((*t++ = *f++) != 0) {
    }
    return r;
}

char *strcat(char *t, const char *f)
{
    strcpy(t + strlen(t), f);
    return t;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c) last = s;
        if (*s == 0) break;
    }
    return (char *)last;
}

unsigned short *wcscpy(unsigned short *t, const unsigned short *f)
{
    unsigned short *r = t;
    while ((*t++ = *f++) != 0) {
    }
    return r;
}

exf_size_t wcslen(const unsigned short *s)
{
    exf_size_t n = 0;
    while (s[n]) n++;
    return n;
}

static unsigned short WLower(unsigned short c)
{
    return (unsigned short)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
}

int _wcsicmp(const unsigned short *a, const unsigned short *b)
{
    while (*a && WLower(*a) == WLower(*b)) { a++; b++; }
    return (int)WLower(*a) - (int)WLower(*b);
}

unsigned long strtoul(const char *s, char **End, int Base)
{
    unsigned long v = 0;
    const char *p = s;
    int d;

    while (isspace((unsigned char)*p)) p++;
    if (Base == 0) {
        Base = 10;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { Base = 16; p += 2; }
    } else if (Base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }
    for (;; p++) {
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10;
        else break;
        if (d >= Base) break;
        v = v * (unsigned long)Base + (unsigned long)d;
    }
    if (End != NULL) *End = (char *)((p == s) ? s : p);
    return v;
}

/* ------------------------------------------------------------------ */
/* Formatted output: %s %c %d %u %x %X %%, with -, 0, width and l     */
/* ------------------------------------------------------------------ */

static int Format(char *Out, const char *f, va_list Args)
{
    char *o = Out;
    char Digits[16];
    const char *Text;
    unsigned long Value;
    int Width, Left, Zero, Len, Neg, Pad;

    for (; *f; f++) {
        if (*f != '%') { *o++ = *f; continue; }
        f++;
        Left = Zero = 0;
        for (;; f++) {
            if (*f == '-') Left = 1;
            else if (*f == '0') Zero = 1;
            else break;
        }
        Width = 0;
        while (*f >= '0' && *f <= '9') Width = Width * 10 + (*f++ - '0');
        while (*f == 'l' || *f == 'h') f++;

        Neg = 0;
        switch (*f) {
        case 's':
            Text = va_arg(Args, const char *);
            if (Text == NULL) Text = "(null)";
            Len = (int)strlen(Text);
            break;
        case 'c':
            Digits[0] = (char)va_arg(Args, int);
            Text = Digits; Len = 1;
            break;
        case 'd': case 'i': case 'u': case 'x': case 'X':
            Value = va_arg(Args, unsigned long);
            if ((*f == 'd' || *f == 'i') && (long)Value < 0) { Neg = 1; Value = (unsigned long)(-(long)Value); }
            Len = 0;
            do {
                unsigned long Base = (*f == 'x' || *f == 'X') ? 16 : 10;
                int d = (int)(Value % Base);
                Digits[sizeof(Digits) - 1 - Len++] = (char)(d < 10 ? '0' + d : (*f == 'x' ? 'a' : 'A') + d - 10);
                Value /= Base;
            } while (Value != 0);
            Text = Digits + sizeof(Digits) - Len;
            break;
        case 0:
            f--;
            continue;
        default:
            *o++ = *f;
            continue;
        }

        Pad = Width - Len - Neg;
        if (!Left && !Zero) while (Pad-- > 0) *o++ = ' ';
        if (Neg) *o++ = '-';
        if (!Left && Zero) while (Pad-- > 0) *o++ = '0';
        while (Len-- > 0) *o++ = *Text++;
        if (Left) while (Pad-- > 0) *o++ = ' ';
    }
    *o = 0;
    return (int)(o - Out);
}

int sprintf(char *Buffer, const char *f, ...)
{
    va_list Args;
    int n;
    va_start(Args, f);
    n = Format(Buffer, f, Args);
    va_end(Args);
    return n;
}

/* The tools print short lines; longer text is cut, never overrun */
int printf(const char *f, ...)
{
    char Buffer[2048];
    char Line[4096];
    va_list Args;
    DWORD Written;
    int n, i, k;

    va_start(Args, f);
    n = Format(Buffer, f, Args);
    va_end(Args);

    /* The console wants CR LF */
    for (i = k = 0; i < n && k < (int)sizeof(Line) - 2; i++) {
        if (Buffer[i] == '\n' && (i == 0 || Buffer[i - 1] != '\r')) Line[k++] = '\r';
        Line[k++] = Buffer[i];
    }
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), Line, (DWORD)k, &Written, NULL);
    return n;
}

int fflush(void *Stream)
{
    (void)Stream;
    return 0;
}

/* A line from standard input (a console or a redirected file) */
char *fgets(char *Buffer, int Size, void *Stream)
{
    HANDLE In = GetStdHandle(STD_INPUT_HANDLE);
    DWORD Got;
    int n = 0;
    char c;

    (void)Stream;
    while (n < Size - 1) {
        if (!ReadFile(In, &c, 1, &Got, NULL) || Got == 0) break;
        Buffer[n++] = c;
        if (c == '\n') break;
    }
    if (n == 0) return NULL;
    Buffer[n] = 0;
    return Buffer;
}

/* ------------------------------------------------------------------ */
/* Start-up: the command line split as the C library does             */
/* ------------------------------------------------------------------ */

#define EXF_MAX_ARGS 32

void __stdcall ExfCrtStart(void)
{
    static char *Argv[EXF_MAX_ARGS + 1];
    char *Line = GetCommandLineA();
    char *Copy = (char *)calloc(strlen(Line) + 1, 1);
    char *p = Line;
    char *o = Copy;
    int Argc = 0;
    int Quote;

    if (Copy == NULL) {
        ExitProcess(3);
    }

    while (*p && Argc < EXF_MAX_ARGS) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == 0) break;
        Argv[Argc++] = o;
        Quote = 0;
        while (*p && (Quote || (*p != ' ' && *p != '\t'))) {
            if (*p == '"') Quote = !Quote;
            else *o++ = *p;
            p++;
        }
        *o++ = 0;
    }
    Argv[Argc] = NULL;

    ExitProcess((UINT)main(Argc, Argv));
}
