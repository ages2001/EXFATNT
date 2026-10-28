/*
 * toolcrt.h - force-included prefix for building exfmt/exfatchk freestanding
 * (see toolcrt.c). Provides the few C-library types and declarations the tools
 * need, so their <stdio.h>/<stdlib.h>/<string.h>/<ctype.h>/<time.h> can be
 * blocked (those pull in CRT-only macros like stderr/toupper). <windows.h> is
 * still used normally for the Win32 API.
 */
#ifndef EXF_TOOLCRT_H
#define EXF_TOOLCRT_H

#ifndef NULL
#define NULL ((void*)0)
#endif
typedef long time_t;

struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year,
        tm_wday, tm_yday, tm_isdst;
};

/* stdio-ish */
extern void *stdout, *stderr;
int   exf_printf(const char *, ...);
int   exf_fprintf(void *, const char *, ...);
int   exf_sprintf(char *, const char *, ...);
int   exf_snprintf(char *, unsigned, const char *, ...);

/* stdlib / string / ctype / time */
void *exf_malloc(unsigned);
void  exf_free(void *);
void *exf_calloc(unsigned, unsigned);
void *exf_realloc(void *, unsigned);
unsigned long exf_strtoul(const char *, char **, int);
long  exf_time(long *);
struct tm *exf_localtime(const time_t *);
int   exf_toupper(int);
int   exf_stricmp(const char *, const char *);
char *strrchr(const char *, int); char *strcat(char *, const char *); long atol(const char *); int atoi(const char *);
extern void *stdin; char *fgets(char *, int, void *); int fflush(void *); int putchar(int); int fputs(const char *, void *); int puts(const char *);
int isalnum(int), isupper(int), islower(int), isxdigit(int); long strtol(const char *, char **, int);
int   tolower(int), isdigit(int), isalpha(int), isspace(int);
void *memset(void *, int, unsigned);
void *memcpy(void *, const void *, unsigned);
void *memmove(void *, const void *, unsigned);
int   memcmp(const void *, const void *, unsigned);
unsigned strlen(const char *);
int   strcmp(const char *, const char *);
int   strncmp(const char *, const char *, unsigned);
char *strcpy(char *, const char *);
char *strncpy(char *, const char *, unsigned);
char *strchr(const char *, int);

#endif
