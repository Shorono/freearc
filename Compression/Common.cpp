#include <stdlib.h>
#include "Common.h"
#include "Compression.h"

// Used in 4x4 only: read entire input buffer before compression begins, allocate output buffer large enough to hold entire compressed output
int compress_all_at_once = 0;

// ��� ��������� ������ �� ��������� ���������� - longjmp ������������� ��������� �������� ������ � ������������ ������
int jmpready = FALSE;
jmp_buf jumper;

// ****************************************************************************
// MEMORY ALLOCATION **********************************************************
// ****************************************************************************

// #define _SZ_ALLOC_DEBUG
/* use _SZ_ALLOC_DEBUG to debug alloc/free operations */
#ifdef _SZ_ALLOC_DEBUG
#include <stdio.h>
int g_allocCount = 0;
int g_allocCountMid = 0;
int g_allocCountBig = 0;
#define alloc_debug_printf(x) fprintf x
#else
#define alloc_debug_printf(x)
#endif

void *MyAlloc(size_t size) throw()
{
  if (size == 0)
    return 0;
  alloc_debug_printf((stderr, "\nAlloc %10d bytes; count = %10d", size, g_allocCount++));
  return ::malloc(size);
}

void MyFree(void *address) throw()
{
  if (address != 0)
    alloc_debug_printf((stderr, "\nFree; count = %10d", --g_allocCount));

  ::free(address);
}

#ifdef FREEARC_WIN

void *MidAlloc(size_t size) throw()
{
  if (size == 0)
    return 0;
  alloc_debug_printf((stderr, "\nAlloc_Mid %10d bytes;  count = %10d", size, g_allocCountMid++));
  return ::VirtualAlloc(0, size, MEM_COMMIT, PAGE_READWRITE);
}

void MidFree(void *address) throw()
{
  if (address == 0)
    return;
  if (address != 0)
  alloc_debug_printf((stderr, "\nFree_Mid; count = %10d", --g_allocCountMid));
  ::VirtualFree(address, 0, MEM_RELEASE);
}

static SIZE_T g_LargePageSize =
    #ifdef _WIN64
    (1 << 21);
    #else
    (1 << 22);
    #endif

typedef SIZE_T (WINAPI *GetLargePageMinimumP)();

int SetLargePageSize()
{
  GetLargePageMinimumP largePageMinimum = (GetLargePageMinimumP)
        ::GetProcAddress(::GetModuleHandle(TEXT("kernel32.dll")), "GetLargePageMinimum");
  if (largePageMinimum == 0)
    return FALSE;
  SIZE_T size = largePageMinimum();
  if (size == 0 || (size & (size - 1)) != 0)
    return FALSE;
  g_LargePageSize = size;
  return TRUE;
}


void *BigAlloc(size_t size) throw()
{
  if (size == 0)
    return 0;
  alloc_debug_printf((stderr, "\nAlloc_Big %10d bytes;  count = %10d", size, g_allocCountBig++));

  if (size > 256*kb)
  {
    void *res = ::VirtualAlloc(0, (size + g_LargePageSize - 1) & (~(g_LargePageSize - 1)),
        MEM_COMMIT | MEM_4MB_PAGES | MEM_TOP_DOWN, PAGE_READWRITE);
    if (res != 0)
      return res;
  }
  return ::VirtualAlloc(0, size, MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE);
}

void BigFree(void *address) throw()
{
  if (address == 0)
    return;
  alloc_debug_printf((stderr, "\nFree_Big; count = %10d", --g_allocCountBig));

  ::VirtualFree(address, 0, MEM_RELEASE);
}

#endif


// ****************************************************************************
// ������� �������� � ���������� **********************************************
// ****************************************************************************

// ���� ������ param �������� ����� ����� - ���������� ���, ����� ���������� error=1
MemSize parseInt (char *param, int *error)
{
  MemSize n=0;
  char c = *param=='='? *++param : *param;
  if (c=='\0') *error=1;
  while (c>='0' && c<='9')  n=n*10+c-'0', c=*++param;
  if (c!='\0') *error=1;
  return n;
}

// ���������� readInt, ������ ������ param ����� ��������� �������� b/k/m/g/^, ��� �������� ��������������� ������� ������ (�� ��������� - '^', �.�. ������� ������)
MemSize parseMem (char *param, int *error)
{
  MemSize n=0;
  char c = *param=='='? *++param : *param;
  if (c=='\0') *error=1;
  while (c>='0' && c<='9')  n=n*10+c-'0', c=*++param;
  switch (c)
  {
    case 'b':  return n;
    case 'k':  return n*kb;
    case 'm':  return n*mb;
    case 'g':  return n*gb;
    case '^':
    case '\0':  return 1<<n;
  }
  *error=1; return 0;
}

// ���������� ��������� �������� ������ ������
void showMem (MemSize mem, char *result)
{
       if (mem%gb==0) sprintf (result, "%dgb", mem/gb);
  else if (mem%mb==0) sprintf (result, "%dmb", mem/mb);
  else if (mem%kb==0) sprintf (result, "%dkb", mem/kb);
  else                sprintf (result, "%ub",  mem);
}

// �������� ������� from � to, �� �� ����� len ��������
void strncopy( char *to, char *from, int len ) {
  for (int i = len; --i && *from; )     *to++ = *from++;
  *to = '\0';
}

// ������� ������ str �� ���������, ���������� �������� splitter.
// ��������� - � ������ str splitter ���������� �� '\0'
//   � ������ result ����������� �������� �� ���������� � str ��������� + NULL (���������� argv)
// ���������� ����� ��������� ��������
int split (char *str, char splitter, char **result_base, int result_size)
{
  char **result      = result_base;
  char **result_last = result_base+result_size-1;
  *result++ = str;
  while (*str && result < result_last)
  {
    while (*str && *str!=splitter) str++;
    if (*str) {
      *str++ = '\0';
      *result++ = str;
    }
  }
  *result = NULL;
  return result-result_base;
}

// �������� � ������ original ��� ��������� from �� to,
// ��������� ����� ���������� new ������ � ���������� ��������, ���� ���� ���� ���� ������
char *subst (char *original, char *from, char *to)
{
  while(1) {
    char *p = strstr (original, from);
    if (!p)  return original;
    char *newstr = new char[strlen(original)+strlen(to)-strlen(from)+1];
    memcpy (newstr, original, p-original);
    strcpy (newstr+(p-original), to);
    strcat (newstr+(p-original), p+strlen(from));
    delete (original);
    original = newstr;
  }
}

// ���������� ������� � ������ ������ � ������� �� � �����, ����������� ������
char *trim_spaces(char *s)
{
  while(isspace(*s)) s++;
  char *last = &last_char(s);
  while(last>=s && isspace(*last))  *last-- = '\0';
  return s;
}

// Replace from:how_many substring and put result in new allocated area
char *str_replace_n (char *orig, char *from, int how_many, char *to)
{
  char *result = new char [strlen(orig) + strlen(to) - how_many + 1], *p=result;
  memcpy(p, orig, from-orig); p += from-orig;
  strcpy(p, to);
  strcat(p, from+how_many);
  return result;
}

// Replace substring and put result in new allocated area
char *str_replace (char *orig, char *from, char *to)
{
  char *p = strstr(orig, from);
  if (p)  return str_replace_n (orig, p, strlen(from), to);
  else    return strdup_msg (orig);
}


// ****************************************************************************
// Windows charset conversion routines ****************************************
// ****************************************************************************

#ifdef FREEARC_WIN
// Converts UTF-8 string to UTF-16
WCHAR *utf8_to_utf16 (const char *utf8, WCHAR *_utf16)
{
  WCHAR *utf16 = _utf16;
  do {
    BYTE c = utf8[0];   UINT c32;
         if (c<=0x7F)   c32 = c;
    else if (c<=0xBF)   c32 = '?';
    else if (c<=0xDF)   c32 = ((c&0x1F) << 6) +  (utf8[1]&0x3F),  utf8++;
    else if (c<=0xEF)   c32 = ((c&0x0F) <<12) + ((utf8[1]&0x3F) << 6) +  (utf8[2]&0x3F),  utf8+=2;
    else                c32 = ((c&0x0F) <<18) + ((utf8[1]&0x3F) <<12) + ((utf8[2]&0x3F) << 6) + (utf8[3]&0x3F),  utf8+=3;

    // Now c32 represents full 32-bit Unicode char
    if (c32 <= 0xFFFF)  *utf16++ = c32;
    else                c32-=0x10000, *utf16++ = c32/0x400 + 0xd800, *utf16++ = c32%0x400 + 0xdc00;

  } while (*utf8++);
  return _utf16;
}

// Converts UTF-16 string to UTF-8
char *utf16_to_utf8 (const WCHAR *utf16, char *_utf8)
{
  char *utf8 = _utf8;
  do {
    UINT c = utf16[0];
    if (0xd800<=c && c<=0xdbff && 0xdc00<=utf16[1] && utf16[1]<=0xdfff)
      c = (c - 0xd800)*0x400 + (UINT)(*++utf16 - 0xdc00) + 0x10000;

    // Now c represents full 32-bit Unicode char
         if (c<=0x7F)   *utf8++ = c;
    else if (c<=0x07FF) *utf8++ = 0xC0|(c>> 6)&0x1F,  *utf8++ = 0x80|(c>> 0)&0x3F;
    else if (c<=0xFFFF) *utf8++ = 0xE0|(c>>12)&0x0F,  *utf8++ = 0x80|(c>> 6)&0x3F,  *utf8++ = 0x80|(c>> 0)&0x3F;
    else                *utf8++ = 0xF0|(c>>18)&0x0F,  *utf8++ = 0x80|(c>>12)&0x3F,  *utf8++ = 0x80|(c>> 6)&0x3F,  *utf8++ = 0x80|(c>> 0)&0x3F;

  } while (*utf16++);
  return _utf8;
}

// Converts UTF-8 string to OEM
char *utf8_to_oem (const char *utf8, char *oem)
{
  WCHAR *utf16 = (WCHAR*) malloc(MY_FILENAME_MAX*4);
  utf8_to_utf16 (utf8, utf16);
  CharToOemW (utf16, oem);
  free (utf16);
  return oem;
}

// Converts OEM string to UTF-8
char *oem_to_utf8 (const char  *oem, char *utf8)
{
  WCHAR *utf16 = (WCHAR*) malloc(MY_FILENAME_MAX*4);
  OemToCharW (oem, utf16);
  utf16_to_utf8 (utf16, utf8);
  free (utf16);
  return utf8;
}
#endif


//*****************************************************************************
// ����� ��������� ���� *******************************************************
//*****************************************************************************

#ifdef FREEARC_WIN
#include <windows.h>

#define LARGE_STRING 1024

TCHAR Saved_Title[LARGE_STRING];
bool Saved = FALSE;

// ���������� ��������� ����������� ����
void EnvSetConsoleTitle (TCHAR *title)
{
  if (!Saved) {
    GetConsoleTitle (Saved_Title, sizeof(Saved_Title));
    Saved = TRUE;
  }
  SetConsoleTitle (title);
}

// ������������ ���������, ������� ��� � ������ ������ ���������
void EnvResetConsoleTitle (void)
{
  if (Saved) {
    SetConsoleTitle (Saved_Title);
    Saved = FALSE;
  }
}

#else // !FREEARC_WIN

void EnvSetConsoleTitle (char *title)
{
  fprintf (stderr, "\033]0;%s\a", title);
}

void EnvResetConsoleTitle (void)    {};

#endif


//*****************************************************************************
// Timing execution ***********************************************************
//*****************************************************************************

#ifdef FREEARC_WIN
// Returns number of wall-clock seconds since some moment
double GetGlobalTime (void)
{
  __int64 freq, t0;
  if( QueryPerformanceFrequency ((LARGE_INTEGER*)&freq) ) {
    QueryPerformanceCounter ((LARGE_INTEGER*)&t0);
    return ((double)t0)/freq;
  } else {
    return ((double)GetTickCount())/1000;
  }
}

// Returns number of seconds spent in this thread
double GetThreadCPUTime (void)
{
    FILETIME kt, ut, x, y;
    int ok = GetThreadTimes(GetCurrentThread(),&x,&y,&kt,&ut);
    return !ok? -1 : ((double) (((long long)(ut.dwHighDateTime) << 32) + ut.dwLowDateTime)) / 10000000;
}
#endif // FREEARC_WIN


#if defined(FREEARC_UNIX) && !defined(FREEARC_NO_TIMING)
// Returns number of wall-clock seconds since some moment
double GetGlobalTime (void)
{
    struct timespec ts;
    int res = clock_gettime(CLOCK_REALTIME, &ts);
    return res? -1 : (ts.tv_sec + ((double)ts.tv_nsec) / 1000000000);
}

// Returns number of seconds spent in this thread
double GetThreadCPUTime (void)
{
    // clock_gettime() gives us per-thread CPU time.  It isn't
    // reliable on Linux, but it's the best we have.
    struct timespec ts;
    int res = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return res? -1 : (ts.tv_sec + ((double)ts.tv_nsec) / 1000000000);
}
#endif // FREEARC_UNIX
