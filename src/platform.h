/* platform.h – everything that depends on the operating system.
 * Implementations: platform_win.c (Windows) and platform_posix.c (Linux).
 * All strings in the program are UTF-8. */
#ifndef DRY_PLATFORM_H
#define DRY_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>

#ifdef _WIN32
#define PATH_SEP '\\'
#else
#define PATH_SEP '/'
#endif

/* Result of reading from input. */
enum {
    READ_INTERRUPTED = -1, /* Ctrl+C */
    READ_EOF = 0,          /* end of input (Ctrl+Z / Ctrl+D) */
    READ_OK = 1
};

void platform_init(void);

/* Program arguments in UTF-8 (on Windows argv uses the ANSI code page). */
char **platform_utf8_argv(int argc, char **argv, int *out_argc);

/* Reads a line (without the line terminator) into a newly allocated buffer.
 * The caller releases it with free() – preferably after zeroing it. */
int platform_read_line(char **line);

/* Reads a password without echoing it; result in buf (NUL-terminated). */
int platform_read_password(char *buf, size_t size);

void platform_write(const char *utf8, size_t len);

/* Clears the screen and the terminal scrollback (if output is a terminal). */
void platform_clear_screen(void);

/* Directory containing the executable (newly allocated string). */
char *platform_exe_dir(void);

FILE *platform_fopen(const char *path, const char *mode);
bool platform_file_exists(const char *path);
/* Flushes the file's buffers all the way to disk. */
bool platform_sync_file(FILE *f);
/* Atomically replaces dst with src. */
bool platform_replace_file(const char *src, const char *dst);
bool platform_remove_file(const char *path);

/* Exclusive lock on a file (created if missing), held until platform_unlock()
 * or the end of the process – even one that crashes. */
enum {
    LOCK_OK,
    LOCK_BUSY, /* another process holds the lock */
    LOCK_ERROR
};
typedef struct PlatformLock PlatformLock;
int platform_lock(const char *path, PlatformLock **out);
void platform_unlock(PlatformLock *lock);

void platform_localtime(time_t t, struct tm *out);

#endif
