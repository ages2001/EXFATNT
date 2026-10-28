/* Wide-string helpers for the freestanding tools (no <windows.h> here, so
   the CRT names do not clash with the SDK's inline declarations). */
unsigned short *wcscpy(unsigned short *d, const unsigned short *s){ unsigned short *r=d; while((*d++=*s++)); return r; }
unsigned wcslen(const unsigned short *s){ const unsigned short *p=s; while(*p)p++; return (unsigned)(p-s); }
unsigned short *wcscat(unsigned short *d, const unsigned short *s){ unsigned short *r=d; while(*d)d++; while((*d++=*s++)); return r; }
int _wcsicmp(const unsigned short *a, const unsigned short *b){ for(;;){ unsigned x=*a++, y=*b++;
    if(x>='a'&&x<='z')x-=32; if(y>='a'&&y<='z')y-=32; if(x!=y||!x) return (int)x-(int)y; } }
