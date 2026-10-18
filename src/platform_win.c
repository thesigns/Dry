/* platform_win.c – platform.h implementation for Windows. */
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>

#include <sodium.h>

#include "platform.h"

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

static HANDLE g_in, g_out;
static bool g_in_console, g_out_console;
static volatile LONG g_interrupted;

static BOOL WINAPI ctrl_handler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        InterlockedExchange(&g_interrupted, 1);
        return TRUE;
    }
    return FALSE;
}

void platform_init(void)
{
    DWORD mode;

    g_in = GetStdHandle(STD_INPUT_HANDLE);
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    g_in_console = GetConsoleMode(g_in, &mode) != 0;
    g_out_console = GetConsoleMode(g_out, &mode) != 0;
    if (g_out_console)
        SetConsoleMode(g_out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
}

static char *wide_to_utf8(const wchar_t *w, int wlen)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, wlen, NULL, 0, NULL, NULL);
    char *s = malloc((size_t)n + 1);
    if (!s)
        return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, wlen, s, n, NULL, NULL);
    s[n] = '\0';
    return s;
}

static wchar_t *utf8_to_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = malloc((size_t)n * sizeof *w);
    if (!w)
        return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

char **platform_utf8_argv(int argc, char **argv, int *out_argc)
{
    int n;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &n);
    char **res;

    (void)argc;
    (void)argv;
    if (!wargv)
        return NULL;
    res = calloc((size_t)n + 1, sizeof *res);
    if (res) {
        for (int i = 0; i < n; i++)
            res[i] = wide_to_utf8(wargv[i], -1);
    }
    LocalFree(wargv);
    *out_argc = n;
    return res;
}

/* Redirected input (file, pipe) – bytes are treated as UTF-8. */
static int read_stream_line(char **line)
{
    size_t cap = 256, len = 0;
    char *buf = malloc(cap);

    if (!buf)
        return READ_EOF;
    for (;;) {
        int c = fgetc(stdin);
        if (c == EOF) {
            if (len == 0) {
                free(buf);
                return READ_EOF;
            }
            break;
        }
        if (c == '\n')
            break;
        if (len + 1 >= cap) {
            char *nbuf = malloc(cap * 2);
            if (!nbuf) {
                sodium_memzero(buf, len);
                free(buf);
                return READ_EOF;
            }
            memcpy(nbuf, buf, len);
            sodium_memzero(buf, len);
            free(buf);
            buf = nbuf;
            cap *= 2;
        }
        buf[len++] = (char)c;
    }
    if (len > 0 && buf[len - 1] == '\r')
        len--;
    buf[len] = '\0';
    *line = buf;
    return READ_OK;
}

/* Console – ReadConsoleW handles non-ASCII characters correctly. */
static int read_console_line(char **line)
{
    size_t cap = 256, len = 0;
    wchar_t *buf = malloc(cap * sizeof *buf);

    if (!buf)
        return READ_EOF;
    for (;;) {
        DWORD got = 0;

        if (cap - len < 64) {
            wchar_t *nbuf = malloc(cap * 2 * sizeof *nbuf);
            if (!nbuf) {
                sodium_memzero(buf, len * sizeof *buf);
                free(buf);
                return READ_EOF;
            }
            memcpy(nbuf, buf, len * sizeof *buf);
            sodium_memzero(buf, len * sizeof *buf);
            free(buf);
            buf = nbuf;
            cap *= 2;
        }
        if (!ReadConsoleW(g_in, buf + len, (DWORD)(cap - len), &got, NULL)) {
            sodium_memzero(buf, len * sizeof *buf);
            free(buf);
            return READ_EOF;
        }
        /* Ctrl+C interrupts the read, returning 0 characters. */
        if (got == 0 || g_interrupted) {
            sodium_memzero(buf, len * sizeof *buf);
            free(buf);
            return READ_INTERRUPTED;
        }
        len += got;
        if (buf[len - 1] == L'\n')
            break;
    }
    while (len > 0 && (buf[len - 1] == L'\n' || buf[len - 1] == L'\r'))
        len--;
    /* Ctrl+Z at the start of a line means end of input. */
    if (len > 0 && buf[0] == 0x1A) {
        free(buf);
        return READ_EOF;
    }
    *line = len > 0 ? wide_to_utf8(buf, (int)len) : calloc(1, 1);
    sodium_memzero(buf, len * sizeof *buf);
    free(buf);
    return *line ? READ_OK : READ_EOF;
}

int platform_read_line(char **line)
{
    if (g_interrupted)
        return READ_INTERRUPTED;
    return g_in_console ? read_console_line(line) : read_stream_line(line);
}

int platform_read_password(char *buf, size_t size)
{
    char *line = NULL;
    size_t len;
    int r;

    if (g_interrupted)
        return READ_INTERRUPTED;
    if (g_in_console) {
        DWORD mode;
        GetConsoleMode(g_in, &mode);
        SetConsoleMode(g_in, mode & ~(DWORD)ENABLE_ECHO_INPUT);
        r = read_console_line(&line);
        SetConsoleMode(g_in, mode);
        platform_write("\n", 1);
    } else {
        r = read_stream_line(&line);
    }
    if (r != READ_OK)
        return r;
    len = strlen(line);
    if (len >= size)
        len = size - 1;
    memcpy(buf, line, len);
    buf[len] = '\0';
    sodium_memzero(line, strlen(line));
    free(line);
    return READ_OK;
}

void platform_write(const char *utf8, size_t len)
{
    if (len == 0)
        return;
    if (g_out_console) {
        int wn = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)len, NULL, 0);
        wchar_t *w = malloc((size_t)wn * sizeof *w);
        DWORD written;
        if (!w)
            return;
        MultiByteToWideChar(CP_UTF8, 0, utf8, (int)len, w, wn);
        WriteConsoleW(g_out, w, (DWORD)wn, &written, NULL);
        sodium_memzero(w, (size_t)wn * sizeof *w);
        free(w);
    } else {
        fwrite(utf8, 1, len, stdout);
        fflush(stdout);
    }
}

void platform_clear_screen(void)
{
    static const char seq[] = "\x1b[H\x1b[2J\x1b[3J";
    if (g_out_console)
        platform_write(seq, sizeof seq - 1);
}

char *platform_exe_dir(void)
{
    DWORD cap = MAX_PATH, n;
    wchar_t *buf = NULL;
    char *res;

    for (;;) {
        wchar_t *nbuf = realloc(buf, cap * sizeof *buf);
        if (!nbuf) {
            free(buf);
            return NULL;
        }
        buf = nbuf;
        n = GetModuleFileNameW(NULL, buf, cap);
        if (n == 0) {
            free(buf);
            return NULL;
        }
        if (n < cap)
            break;
        cap *= 2;
    }
    while (n > 0 && buf[n - 1] != L'\\' && buf[n - 1] != L'/')
        n--;
    if (n > 0)
        n--; /* drop the trailing separator */
    res = wide_to_utf8(buf, (int)n);
    free(buf);
    return res;
}

FILE *platform_fopen(const char *path, const char *mode)
{
    wchar_t *wpath = utf8_to_wide(path), *wmode = utf8_to_wide(mode);
    FILE *f = NULL;
    if (wpath && wmode)
        f = _wfopen(wpath, wmode);
    free(wpath);
    free(wmode);
    return f;
}

bool platform_file_exists(const char *path)
{
    wchar_t *wpath = utf8_to_wide(path);
    DWORD attr;
    if (!wpath)
        return false;
    attr = GetFileAttributesW(wpath);
    free(wpath);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool platform_sync_file(FILE *f)
{
    if (fflush(f) != 0)
        return false;
    return FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(f))) != 0;
}

bool platform_replace_file(const char *src, const char *dst)
{
    wchar_t *wsrc = utf8_to_wide(src), *wdst = utf8_to_wide(dst);
    bool ok = wsrc && wdst &&
              MoveFileExW(wsrc, wdst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    free(wsrc);
    free(wdst);
    return ok;
}

bool platform_remove_file(const char *path)
{
    wchar_t *wpath = utf8_to_wide(path);
    bool ok = wpath && DeleteFileW(wpath);
    free(wpath);
    return ok;
}

struct PlatformLock {
    HANDLE h;
};

/* No sharing allowed: a second open fails with ERROR_SHARING_VIOLATION.
 * Windows deletes the file once the handle is closed, also after a crash. */
int platform_lock(const char *path, PlatformLock **out)
{
    wchar_t *wpath = utf8_to_wide(path);
    PlatformLock *lock;
    HANDLE h;

    if (!wpath)
        return LOCK_ERROR;
    h = CreateFileW(wpath, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    free(wpath);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_SHARING_VIOLATION ? LOCK_BUSY : LOCK_ERROR;
    lock = malloc(sizeof *lock);
    if (!lock) {
        CloseHandle(h);
        return LOCK_ERROR;
    }
    lock->h = h;
    *out = lock;
    return LOCK_OK;
}

void platform_unlock(PlatformLock *lock)
{
    if (lock) {
        CloseHandle(lock->h);
        free(lock);
    }
}

void platform_localtime(time_t t, struct tm *out)
{
    localtime_s(out, &t);
}

#else
typedef int platform_win_unused;
#endif
