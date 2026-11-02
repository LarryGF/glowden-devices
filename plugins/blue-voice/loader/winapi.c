#define _GNU_SOURCE
#include "pe.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

typedef int BOOL;
typedef uint32_t DWORD;
typedef void *HANDLE;

#define FAKE_HANDLE ((HANDLE)0x1000)
#define INVALID_HANDLE ((HANDLE)-1)

static int verbose(void)
{
    static int v = -1;
    if (v < 0) v = getenv("PELOADER_VERBOSE") != NULL;
    return v;
}

static size_t wlen(const WCHAR *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void wprint(const WCHAR *s, size_t n)
{
    for (size_t i = 0; i < n; i++) fputc(s[i] < 128 ? s[i] : '?', stderr);
}

/* ---- errors ---- */
static __thread DWORD last_error;
static DWORD WINAPI GetLastError(void) { return last_error; }
static void WINAPI SetLastError(DWORD e) { last_error = e; }

/* ---- heap: 16-byte header keeps the requested size for HeapSize ---- */
static void *heap_alloc(size_t n, int zero)
{
    uint8_t *p = zero ? calloc(1, n + 16) : malloc(n + 16);
    if (!p) return NULL;
    *(size_t *)p = n;
    return p + 16;
}
static HANDLE WINAPI GetProcessHeap(void) { return FAKE_HANDLE; }
static void *WINAPI HeapAlloc(HANDLE h, DWORD flags, size_t n) { (void)h; return heap_alloc(n, flags & 8); }
static BOOL WINAPI HeapFree(HANDLE h, DWORD f, void *p) { (void)h; (void)f; if (p) free((uint8_t *)p - 16); return 1; }
static size_t WINAPI HeapSize(HANDLE h, DWORD f, const void *p) { (void)h; (void)f; return *(size_t *)((uint8_t *)p - 16); }
static void *WINAPI HeapReAlloc(HANDLE h, DWORD flags, void *p, size_t n)
{
    (void)h;
    if (!p) return heap_alloc(n, flags & 8);
    size_t old = *(size_t *)((uint8_t *)p - 16);
    uint8_t *q = realloc((uint8_t *)p - 16, n + 16);
    if (!q) return NULL;
    if ((flags & 8) && n > old) memset(q + 16 + old, 0, n - old);
    *(size_t *)q = n;
    return q + 16;
}
static BOOL WINAPI HeapDestroy(HANDLE h) { (void)h; return 1; }
static HANDLE WINAPI LocalFree(HANDLE p) { (void)p; return NULL; }

/* ---- critical sections: Windows CRITICAL_SECTION is 40 bytes, same as pthread_mutex_t ---- */
_Static_assert(sizeof(pthread_mutex_t) <= 40, "mutex must fit CRITICAL_SECTION");
static void cs_init(void *cs)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(cs, &a);
}
static void WINAPI InitializeCriticalSection(void *cs) { cs_init(cs); }
static BOOL WINAPI InitializeCriticalSectionEx(void *cs, DWORD spin, DWORD f) { (void)spin; (void)f; cs_init(cs); return 1; }
static BOOL WINAPI InitializeCriticalSectionAndSpinCount(void *cs, DWORD spin) { (void)spin; cs_init(cs); return 1; }
static void WINAPI EnterCriticalSection(void *cs) { pthread_mutex_lock(cs); }
static void WINAPI LeaveCriticalSection(void *cs) { pthread_mutex_unlock(cs); }
static void WINAPI DeleteCriticalSection(void *cs) { pthread_mutex_destroy(cs); }

/* ---- TLS slots ---- */
#define TLS_SLOTS 1088
static __thread void *tls_values[TLS_SLOTS];
static uint8_t tls_used[TLS_SLOTS];
static pthread_mutex_t tls_lock = PTHREAD_MUTEX_INITIALIZER;
static DWORD WINAPI TlsAlloc(void)
{
    pthread_mutex_lock(&tls_lock);
    for (DWORD i = 0; i < TLS_SLOTS; i++)
        if (!tls_used[i]) { tls_used[i] = 1; pthread_mutex_unlock(&tls_lock); return i; }
    pthread_mutex_unlock(&tls_lock);
    return 0xffffffff;
}
static void *WINAPI TlsGetValue(DWORD i) { last_error = 0; return i < TLS_SLOTS ? tls_values[i] : NULL; }
static BOOL WINAPI TlsSetValue(DWORD i, void *v) { if (i >= TLS_SLOTS) return 0; tls_values[i] = v; return 1; }
static BOOL WINAPI TlsFree(DWORD i) { if (i < TLS_SLOTS) tls_used[i] = 0; return 1; }

/* ---- SList (only init/flush are imported) ---- */
static pthread_mutex_t slist_lock = PTHREAD_MUTEX_INITIALIZER;
static void WINAPI InitializeSListHead(void *h) { memset(h, 0, 16); }
static void *WINAPI InterlockedFlushSList(void *h)
{
    pthread_mutex_lock(&slist_lock);
    void *first = *(void **)h;
    memset(h, 0, 16);
    pthread_mutex_unlock(&slist_lock);
    return first;
}

/* ---- time / ids ---- */
static BOOL WINAPI QueryPerformanceCounter(int64_t *c)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    *c = ts.tv_sec * 10000000LL + ts.tv_nsec / 100;
    return 1;
}
static BOOL WINAPI QueryPerformanceFrequency(int64_t *f) { *f = 10000000; return 1; }
static void WINAPI GetSystemTimeAsFileTime(uint64_t *ft)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    *ft = (ts.tv_sec + 11644473600ULL) * 10000000ULL + ts.tv_nsec / 100;
}
static void WINAPI GetLocalTime(uint16_t *st)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    uint16_t v[8] = {tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_wday, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, 0};
    memcpy(st, v, sizeof v);
}
static DWORD WINAPI GetCurrentProcessId(void) { return getpid(); }
static DWORD WINAPI GetCurrentThreadId(void) { return syscall(SYS_gettid); }
static HANDLE WINAPI GetCurrentProcess(void) { return INVALID_HANDLE; }

/* ---- process / CRT plumbing ---- */
static BOOL WINAPI IsProcessorFeaturePresent(DWORD f) { return f != 23; /* no __fastfail */ }
static BOOL WINAPI IsDebuggerPresent(void) { return 0; }
static void WINAPI GetStartupInfoW(void *si) { memset(si, 0, 104); *(DWORD *)si = 104; }
static void *WINAPI EncodePointer(void *p) { return p; }
static void *WINAPI DecodePointer(void *p) { return p; }
static WCHAR cmdline_w[] = {'a', 'p', 'o', 0};
static char cmdline_a[] = "apo";
static WCHAR *WINAPI GetCommandLineW(void) { return cmdline_w; }
static char *WINAPI GetCommandLineA(void) { return cmdline_a; }
static WCHAR *WINAPI GetEnvironmentStringsW(void) { static WCHAR env[2]; return env; }
static BOOL WINAPI FreeEnvironmentStringsW(WCHAR *e) { (void)e; return 1; }
static void *WINAPI SetUnhandledExceptionFilter(void *f) { (void)f; return NULL; }
static long WINAPI UnhandledExceptionFilter(void *e) { (void)e; return 0; }
static BOOL WINAPI TerminateProcess(HANDLE h, unsigned code) { (void)h; fprintf(stderr, "peloader: TerminateProcess(%u)\n", code); abort(); }
static void WINAPI ExitProcess(unsigned code) { fprintf(stderr, "peloader: ExitProcess(%u)\n", code); _exit(code); }
static void WINAPI RaiseException(DWORD code, DWORD flags, DWORD n, const uintptr_t *args)
{
    (void)flags; (void)args;
    fprintf(stderr, "peloader: DLL raised exception 0x%08x (%u args)\n", code, n);
    abort();
}
static void WINAPI RtlCaptureContext(void *ctx) { memset(ctx, 0, 1232); }
static void *WINAPI RtlLookupFunctionEntry(uint64_t pc, uint64_t *base, void *h) { (void)pc; (void)h; *base = 0; return NULL; }

/* ---- modules ---- */
static uint8_t *dll_base;
/* Named system modules get a fake handle; GetProcAddress resolves against our stand-ins. */
static HANDLE WINAPI GetModuleHandleW(const WCHAR *name) { return name ? FAKE_HANDLE : (HANDLE)dll_base; }
static BOOL WINAPI GetModuleHandleExW(DWORD flags, const WCHAR *name, HANDLE *out) { (void)flags; (void)name; *out = dll_base; return 1; }
static DWORD WINAPI GetModuleFileNameW(HANDLE m, WCHAR *buf, DWORD n)
{
    (void)m;
    static const char path[] = "C:\\apo\\apo.dll";
    DWORD i = 0;
    for (; i + 1 < n && path[i]; i++) buf[i] = path[i];
    if (n) buf[i] = 0;
    return i;
}
static HANDLE WINAPI LoadLibraryExW(const WCHAR *name, HANDLE f, DWORD flags) { (void)name; (void)f; (void)flags; return NULL; }
static BOOL WINAPI FreeLibrary(HANDLE m) { (void)m; return 1; }
static void *WINAPI GetProcAddress(HANDLE m, const char *name)
{
    (void)m;
    if ((uintptr_t)name < 0x10000) return NULL;
    return winapi_resolve("kernel32.dll", name, 0);
}
static void *WINAPI RtlPcToFileHeader(void *pc, void **base) { (void)pc; *base = dll_base; return dll_base; }

/* ---- resources: the engine doesn't need them ---- */
static HANDLE WINAPI FindResourceW(HANDLE m, const WCHAR *n, const WCHAR *t) { (void)m; (void)n; (void)t; return NULL; }
static HANDLE WINAPI FindResourceExW(HANDLE m, const WCHAR *t, const WCHAR *n, uint16_t l) { (void)m; (void)t; (void)n; (void)l; return NULL; }
static HANDLE WINAPI LoadResource(HANDLE m, HANDLE r) { (void)m; (void)r; return NULL; }
static void *WINAPI LockResource(HANDLE r) { (void)r; return NULL; }
static DWORD WINAPI SizeofResource(HANDLE m, HANDLE r) { (void)m; (void)r; return 0; }

/* ---- code pages / strings: ASCII-only is enough for this DLL ---- */
static unsigned WINAPI GetACP(void) { return 1252; }
static unsigned WINAPI GetOEMCP(void) { return 437; }
static unsigned WINAPI GetConsoleOutputCP(void) { return 65001; }
static BOOL WINAPI IsValidCodePage(unsigned cp) { (void)cp; return 1; }
static BOOL WINAPI GetCPInfo(unsigned cp, uint8_t *info)
{
    (void)cp;
    memset(info, 0, 20);
    *(DWORD *)info = 1;
    info[4] = '?';
    return 1;
}
static int WINAPI MultiByteToWideChar(unsigned cp, DWORD f, const char *s, int n, WCHAR *out, int outn)
{
    (void)cp; (void)f;
    if (n < 0) n = strlen(s) + 1;
    if (!outn) return n;
    if (outn < n) { last_error = 122; return 0; }
    for (int i = 0; i < n; i++) out[i] = (uint8_t)s[i];
    return n;
}
static int WINAPI WideCharToMultiByte(unsigned cp, DWORD f, const WCHAR *s, int n, char *out, int outn, const char *def, BOOL *used)
{
    (void)cp; (void)f; (void)def;
    if (used) *used = 0;
    if (n < 0) n = wlen(s) + 1;
    if (!outn) return n;
    if (outn < n) { last_error = 122; return 0; }
    for (int i = 0; i < n; i++) out[i] = s[i] < 256 ? s[i] : '?';
    return n;
}
static int WINAPI lstrcmpiW(const WCHAR *a, const WCHAR *b)
{
    for (;; a++, b++) {
        WCHAR x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y || !x) return x < y ? -1 : x > y;
    }
}
static WCHAR *WINAPI CharNextW(const WCHAR *p) { return (WCHAR *)(*p ? p + 1 : p); }
static int lcmap(const WCHAR *s, int n, WCHAR *out, int outn, DWORD flags)
{
    if (n < 0) n = wlen(s) + 1;
    if (!outn) return n;
    if (outn < n) { last_error = 122; return 0; }
    for (int i = 0; i < n; i++) {
        WCHAR c = s[i];
        if ((flags & 0x100) && c >= 'A' && c <= 'Z') c += 32;
        if ((flags & 0x200) && c >= 'a' && c <= 'z') c -= 32;
        out[i] = c;
    }
    return n;
}
static int WINAPI LCMapStringEx(const WCHAR *loc, DWORD flags, const WCHAR *s, int n, WCHAR *out, int outn, void *a, void *b, intptr_t c)
{
    (void)loc; (void)a; (void)b; (void)c;
    return lcmap(s, n, out, outn, flags);
}
static int WINAPI LCMapStringW(DWORD lcid, DWORD flags, const WCHAR *s, int n, WCHAR *out, int outn)
{
    (void)lcid;
    return lcmap(s, n, out, outn, flags);
}
static BOOL WINAPI GetStringTypeW(DWORD type, const WCHAR *s, int n, uint16_t *out)
{
    (void)type;
    if (n < 0) n = wlen(s) + 1;
    for (int i = 0; i < n; i++) {
        WCHAR c = s[i];
        uint16_t t = 0;
        if (c >= 'A' && c <= 'Z') t |= 0x101;
        if (c >= 'a' && c <= 'z') t |= 0x102;
        if (c >= '0' && c <= '9') t |= 0x84;
        if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) t |= 0x80;
        if (c == ' ' || (c >= 9 && c <= 13)) t |= 0x08;
        if (c == ' ' || c == 9) t |= 0x40;
        if (c < 32 || c == 127) t |= 0x20;
        if (c > 32 && c < 127 && !(t & 0x107)) t |= 0x10;
        out[i] = t;
    }
    return 1;
}
static DWORD WINAPI GetUserDefaultLCID(void) { return 0x409; }
static DWORD WINAPI GetThreadLocale(void) { return 0x409; }
static BOOL WINAPI SetThreadLocale(DWORD l) { (void)l; return 1; }
static BOOL WINAPI IsValidLocale(DWORD l, DWORD f) { (void)l; (void)f; return 1; }
static BOOL WINAPI EnumSystemLocalesW(void *cb, DWORD f) { (void)cb; (void)f; return 1; }
static int WINAPI GetLocaleInfoW(DWORD l, DWORD t, WCHAR *out, int n) { (void)l; (void)t; if (n) out[0] = 0; return 1; }

/* ---- files / console: no file access; stdout/stderr go to stderr ---- */
static HANDLE WINAPI GetStdHandle(DWORD n) { return (HANDLE)(uintptr_t)(0x2000 + (n & 0xf)); }
static BOOL WINAPI SetStdHandle(DWORD n, HANDLE h) { (void)n; (void)h; return 1; }
static DWORD WINAPI GetFileType(HANDLE h) { return (uintptr_t)h >= 0x2000 && (uintptr_t)h < 0x2010 ? 2 : 0; }
static BOOL WINAPI GetConsoleMode(HANDLE h, DWORD *m) { (void)h; (void)m; return 0; }
static BOOL WINAPI WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, void *ov)
{
    (void)ov;
    if (GetFileType(h) == 2 && verbose()) fwrite(buf, 1, n, stderr);
    if (written) *written = n;
    return 1;
}
static BOOL WINAPI WriteConsoleW(HANDLE h, const WCHAR *buf, DWORD n, DWORD *written, void *r)
{
    (void)h; (void)r;
    if (verbose()) wprint(buf, n);
    if (written) *written = n;
    return 1;
}
static BOOL WINAPI ReadConsoleW(HANDLE h, void *b, DWORD n, DWORD *rd, void *c) { (void)h; (void)b; (void)n; (void)c; if (rd) *rd = 0; return 0; }
static HANDLE WINAPI CreateFileW(const WCHAR *n, DWORD a, DWORD s, void *sa, DWORD d, DWORD f, HANDLE t)
{
    (void)a; (void)s; (void)sa; (void)d; (void)f; (void)t;
    if (verbose()) { fputs("peloader: CreateFileW(", stderr); wprint(n, wlen(n)); fputs(") denied\n", stderr); }
    last_error = 5;
    return INVALID_HANDLE;
}
static HANDLE WINAPI CreateFileA(const char *n, DWORD a, DWORD s, void *sa, DWORD d, DWORD f, HANDLE t)
{
    (void)a; (void)s; (void)sa; (void)d; (void)f; (void)t;
    if (verbose()) fprintf(stderr, "peloader: CreateFileA(%s) denied\n", n);
    last_error = 5;
    return INVALID_HANDLE;
}
static BOOL WINAPI ReadFile(HANDLE h, void *b, DWORD n, DWORD *rd, void *ov) { (void)h; (void)b; (void)n; (void)ov; if (rd) *rd = 0; return 0; }
static DWORD WINAPI GetFileSize(HANDLE h, DWORD *hi) { (void)h; if (hi) *hi = 0; return 0xffffffff; }
static BOOL WINAPI GetFileSizeEx(HANDLE h, int64_t *s) { (void)h; *s = 0; return 0; }
static BOOL WINAPI SetFilePointerEx(HANDLE h, int64_t d, int64_t *o, DWORD m) { (void)h; (void)d; (void)o; (void)m; return 0; }
static BOOL WINAPI SetEndOfFile(HANDLE h) { (void)h; return 0; }
static BOOL WINAPI FlushFileBuffers(HANDLE h) { (void)h; return 1; }
static HANDLE WINAPI FindFirstFileExW(const WCHAR *n, int l, void *d, int s, void *f, DWORD fl) { (void)n; (void)l; (void)d; (void)s; (void)f; (void)fl; last_error = 2; return INVALID_HANDLE; }
static BOOL WINAPI FindNextFileW(HANDLE h, void *d) { (void)h; (void)d; return 0; }
static BOOL WINAPI FindClose(HANDLE h) { (void)h; return 1; }
static BOOL WINAPI CloseHandle(HANDLE h) { (void)h; return 1; }
static void WINAPI OutputDebugStringA(const char *s) { if (verbose()) fprintf(stderr, "[dll] %s", s); }
static void WINAPI OutputDebugStringW(const WCHAR *s) { if (verbose()) { fputs("[dll] ", stderr); wprint(s, wlen(s)); } }

/* ---- sync objects / shared memory: single process, nothing to share with ---- */
static HANDLE WINAPI CreateEventW(void *sa, BOOL m, BOOL i, const WCHAR *n) { (void)sa; (void)m; (void)i; (void)n; return FAKE_HANDLE; }
static BOOL WINAPI SetEvent(HANDLE h) { (void)h; return 1; }
static BOOL WINAPI ResetEvent(HANDLE h) { (void)h; return 1; }
static DWORD WINAPI WaitForSingleObject(HANDLE h, DWORD ms) { (void)h; (void)ms; return 0; }
static DWORD WINAPI WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL a) { (void)h; (void)ms; (void)a; return 0; }
static HANDLE WINAPI CreateMutexW(void *sa, BOOL o, const WCHAR *n) { (void)sa; (void)o; (void)n; return FAKE_HANDLE; }
static HANDLE WINAPI OpenMutexW(DWORD a, BOOL i, const WCHAR *n) { (void)a; (void)i; (void)n; last_error = 2; return NULL; }
static BOOL WINAPI ReleaseMutex(HANDLE h) { (void)h; return 1; }
static HANDLE WINAPI CreateFileMappingW(HANDLE f, void *sa, DWORD p, DWORD hi, DWORD lo, const WCHAR *n) { (void)f; (void)sa; (void)p; (void)hi; (void)lo; (void)n; last_error = 5; return NULL; }
static HANDLE WINAPI OpenFileMappingW(DWORD a, BOOL i, const WCHAR *n) { (void)a; (void)i; (void)n; last_error = 2; return NULL; }
static void *WINAPI MapViewOfFile(HANDLE h, DWORD a, DWORD hi, DWORD lo, size_t n) { (void)h; (void)a; (void)hi; (void)lo; (void)n; return NULL; }
static BOOL WINAPI UnmapViewOfFile(const void *p) { (void)p; return 1; }

/* ---- advapi32: no registry ---- */
static long WINAPI RegFail(void) { return 2; /* ERROR_FILE_NOT_FOUND */ }
static BOOL WINAPI SecFail(void) { last_error = 1; return 0; }

/* ---- ole32 / oleaut32 ---- */
static void *WINAPI CoTaskMemAlloc(size_t n) { return heap_alloc(n, 0); }
static void *WINAPI CoTaskMemRealloc(void *p, size_t n) { return HeapReAlloc(NULL, 0, p, n); }
static void WINAPI CoTaskMemFree(void *p) { HeapFree(NULL, 0, p); }
static long WINAPI PropVariantClear(void *pv) { memset(pv, 0, 24); return 0; }
static long WINAPI CoCreateInstance(void *a, void *b, DWORD c, void *d, void **out) { (void)a; (void)b; (void)c; (void)d; *out = NULL; return 0x80004001; }
static int WINAPI StringFromGUID2(const uint8_t *g, WCHAR *out, int n)
{
    char buf[40];
    snprintf(buf, sizeof buf, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             *(uint32_t *)g, *(uint16_t *)(g + 4), *(uint16_t *)(g + 6), g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    if (n < 39) return 0;
    for (int i = 0; i < 39; i++) out[i] = buf[i];
    return 39;
}
static WCHAR *WINAPI SysAllocString(const WCHAR *s)
{
    if (!s) return NULL;
    size_t n = wlen(s);
    uint8_t *p = heap_alloc(4 + (n + 1) * 2, 0);
    *(uint32_t *)p = n * 2;
    memcpy(p + 4, s, (n + 1) * 2);
    return (WCHAR *)(p + 4);
}
static void WINAPI SysFreeString(WCHAR *s) { if (s) HeapFree(NULL, 0, (uint8_t *)s - 4); }
static unsigned WINAPI SysStringLen(WCHAR *s) { return s ? *(uint32_t *)((uint8_t *)s - 4) / 2 : 0; }
static long WINAPI OleFail(void) { return 0x80004005; }

#define F(name) {#name, (void *)name}
static const struct { const char *name; void *fn; } kernel32[] = {
    F(GetLastError), F(SetLastError), F(GetProcessHeap), F(HeapAlloc), F(HeapFree), F(HeapSize), F(HeapReAlloc),
    F(HeapDestroy), F(LocalFree), F(InitializeCriticalSection), F(InitializeCriticalSectionEx),
    F(InitializeCriticalSectionAndSpinCount), F(EnterCriticalSection), F(LeaveCriticalSection),
    F(DeleteCriticalSection), F(TlsAlloc), F(TlsGetValue), F(TlsSetValue), F(TlsFree), F(InitializeSListHead),
    F(InterlockedFlushSList), F(QueryPerformanceCounter), F(QueryPerformanceFrequency), F(GetSystemTimeAsFileTime),
    F(GetLocalTime), F(GetCurrentProcessId), F(GetCurrentThreadId), F(GetCurrentProcess),
    F(IsProcessorFeaturePresent), F(IsDebuggerPresent), F(GetStartupInfoW), F(EncodePointer), F(DecodePointer),
    F(GetCommandLineW), F(GetCommandLineA), F(GetEnvironmentStringsW), F(FreeEnvironmentStringsW),
    F(SetUnhandledExceptionFilter), F(UnhandledExceptionFilter), F(TerminateProcess), F(ExitProcess),
    F(RaiseException), F(RtlCaptureContext), F(RtlLookupFunctionEntry), F(GetModuleHandleW), F(GetModuleHandleExW),
    F(GetModuleFileNameW), F(LoadLibraryExW), F(FreeLibrary), F(GetProcAddress), F(RtlPcToFileHeader),
    F(FindResourceW), F(FindResourceExW), F(LoadResource), F(LockResource), F(SizeofResource), F(GetACP),
    F(GetOEMCP), F(GetConsoleOutputCP), F(IsValidCodePage), F(GetCPInfo), F(MultiByteToWideChar),
    F(WideCharToMultiByte), F(lstrcmpiW), F(LCMapStringEx), F(LCMapStringW), F(GetStringTypeW),
    F(GetUserDefaultLCID), F(GetThreadLocale), F(SetThreadLocale), F(IsValidLocale), F(EnumSystemLocalesW),
    F(GetLocaleInfoW), F(GetStdHandle), F(SetStdHandle), F(GetFileType), F(GetConsoleMode), F(WriteFile),
    F(WriteConsoleW), F(ReadConsoleW), F(CreateFileW), F(CreateFileA), F(ReadFile), F(GetFileSize),
    F(GetFileSizeEx), F(SetFilePointerEx), F(SetEndOfFile), F(FlushFileBuffers), F(FindFirstFileExW),
    F(FindNextFileW), F(FindClose), F(CloseHandle), F(OutputDebugStringA), F(OutputDebugStringW), F(CreateEventW),
    F(SetEvent), F(ResetEvent), F(WaitForSingleObject), F(WaitForSingleObjectEx), F(CreateMutexW), F(OpenMutexW),
    F(ReleaseMutex), F(CreateFileMappingW), F(OpenFileMappingW), F(MapViewOfFile), F(UnmapViewOfFile),
    F(CharNextW), F(CoTaskMemAlloc), F(CoTaskMemRealloc), F(CoTaskMemFree), F(PropVariantClear),
    F(CoCreateInstance), F(StringFromGUID2),
};
#undef F

static const char *const advapi_fail[] = {
    "RegDeleteKeyExW", "RegSetValueExW", "RegQueryInfoKeyW", "RegOpenKeyExW", "RegEnumKeyExW", "RegDeleteValueW",
    "RegDeleteKeyW", "RegCreateKeyExW", "RegCloseKey",
};

void *winapi_resolve(const char *dll, const char *name, uint16_t ordinal)
{
    if (!strcasecmp(dll, "OLEAUT32.dll")) {
        switch (ordinal) {
        case 2: return (void *)SysAllocString;
        case 6: return (void *)SysFreeString;
        case 7: return (void *)SysStringLen;
        default: return (void *)OleFail;
        }
    }
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof kernel32 / sizeof *kernel32; i++)
        if (!strcmp(kernel32[i].name, name)) return kernel32[i].fn;
    if (!strcasecmp(dll, "ADVAPI32.dll")) {
        for (size_t i = 0; i < sizeof advapi_fail / sizeof *advapi_fail; i++)
            if (!strcmp(advapi_fail[i], name)) return (void *)RegFail;
        if (!strncmp(name, "GetSecurityDescriptor", 21) || !strcmp(name, "MakeSelfRelativeSD") ||
            !strcmp(name, "ConvertStringSecurityDescriptorToSecurityDescriptorW"))
            return (void *)SecFail;
    }
    return NULL;
}

void winapi_set_module(uint8_t *base) { dll_base = base; }
