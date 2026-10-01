/*
 * EXFCRT - the few C library routines the tools use, on kernel32 only.
 *
 * The NT 3.51/4.0 builds (NT\build.bat) of exfmt, exfatchk and exfinst use
 * this instead of the Visual C++ C library, whose start-up code imports
 * kernel32 routines NT 3.1 does not have. One binary then runs on
 * NT 3.1 to XP. Defined by EXF_OWN_CRT; the WDK builds use msvcrt.dll.
 */

#ifndef _EXFCRT_H_
#define _EXFCRT_H_

#include <stdarg.h>

/* Visual C++ 4.x's windows.h brings in ctype.h, whose macros use the C library */
#undef isalpha
#undef isdigit
#undef isspace
#undef isupper
#undef islower
#undef isalnum
#undef isxdigit
#undef toupper
#undef tolower
#undef stdin
#undef stdout
#undef stderr

typedef unsigned int exf_size_t;

#define stdin   ((void *)0)
#define stdout  ((void *)1)
#define stderr  ((void *)2)

int printf(const char *Format, ...);
int sprintf(char *Buffer, const char *Format, ...);
int fflush(void *Stream);
char *fgets(char *Buffer, int Size, void *Stream);

void *malloc(exf_size_t Size);
void *calloc(exf_size_t Count, exf_size_t Size);
void free(void *Block);
unsigned long strtoul(const char *Text, char **End, int Base);

void *memcpy(void *To, const void *From, exf_size_t Length);
void *memset(void *To, int Value, exf_size_t Length);
int memcmp(const void *A, const void *B, exf_size_t Length);
exf_size_t strlen(const char *Text);
int strcmp(const char *A, const char *B);
char *strcpy(char *To, const char *From);
char *strcat(char *To, const char *From);
char *strrchr(const char *Text, int Char);
int _stricmp(const char *A, const char *B);
unsigned short *wcscpy(unsigned short *To, const unsigned short *From);
exf_size_t wcslen(const unsigned short *Text);
int _wcsicmp(const unsigned short *A, const unsigned short *B);

int toupper(int Char);
int tolower(int Char);
int isalpha(int Char);
int isdigit(int Char);
int isspace(int Char);

#endif
