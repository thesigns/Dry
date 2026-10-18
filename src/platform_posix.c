/* platform_posix.c – platform.h implementation for Linux. */
#ifndef _WIN32

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <sodium.h>

#include "platform.h"

static volatile sig_atomic_t g_interrupted;

static void sigint_handler(int sig)
{
    (void)sig;
    g_interrupted = 1;
}

void platform_init(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART – reading must be interrupted */
    sigaction(SIGINT, &sa, NULL);
}

char **platform_utf8_argv(int argc, char **argv, int *out_argc)
{
    *out_argc = argc;
    return argv;
}

int platform_read_line(char **line)
{
    char *buf = NULL;
    size_t cap = 0;
    ssize_t len;

    if (g_interrupted)
        return READ_INTERRUPTED;
    len = getline(&buf, &cap, stdin);
    if (len < 0) {
        free(buf);
        return (g_interrupted || errno == EINTR) ? READ_INTERRUPTED : READ_EOF;
    }
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';
    *line = buf;
    return READ_OK;
}

int platform_read_password(char *buf, size_t size)
{
    struct termios old, noecho;
    bool tty = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old) == 0;
    char *line = NULL;
    size_t len;
    int r;

    if (tty) {
        noecho = old;
        noecho.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &noecho);
    }
    r = platform_read_line(&line);
    if (tty) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
        platform_write("\n", 1);
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
    fwrite(utf8, 1, len, stdout);
    fflush(stdout);
}

void platform_clear_screen(void)
{
    static const char seq[] = "\x1b[H\x1b[2J\x1b[3J";
    if (isatty(STDOUT_FILENO))
        platform_write(seq, sizeof seq - 1);
}

char *platform_exe_dir(void)
{
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    char *res;

    if (n <= 0)
        n = 1, buf[0] = '.';
    else {
        while (n > 0 && buf[n - 1] != '/')
            n--;
        if (n > 1)
            n--; /* drop the trailing separator (but keep a bare "/") */
    }
    res = malloc((size_t)n + 1);
    if (!res)
        return NULL;
    memcpy(res, buf, (size_t)n);
    res[n] = '\0';
    return res;
}

FILE *platform_fopen(const char *path, const char *mode)
{
    return fopen(path, mode);
}

bool platform_file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool platform_sync_file(FILE *f)
{
    return fflush(f) == 0 && fsync(fileno(f)) == 0;
}

bool platform_replace_file(const char *src, const char *dst)
{
    return rename(src, dst) == 0;
}

bool platform_remove_file(const char *path)
{
    return remove(path) == 0;
}

struct PlatformLock {
    int fd;
};

/* A POSIX record lock is released by the kernel when the process exits.
 * The lock file itself stays in place: unlinking it would race with other processes. */
int platform_lock(const char *path, PlatformLock **out)
{
    struct flock fl;
    PlatformLock *lock;
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);

    if (fd < 0)
        return LOCK_ERROR;
    memset(&fl, 0, sizeof fl);
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &fl) != 0) {
        int busy = errno == EACCES || errno == EAGAIN;
        close(fd);
        return busy ? LOCK_BUSY : LOCK_ERROR;
    }
    lock = malloc(sizeof *lock);
    if (!lock) {
        close(fd);
        return LOCK_ERROR;
    }
    lock->fd = fd;
    *out = lock;
    return LOCK_OK;
}

void platform_unlock(PlatformLock *lock)
{
    if (lock) {
        close(lock->fd);
        free(lock);
    }
}

void platform_localtime(time_t t, struct tm *out)
{
    localtime_r(&t, out);
}

#else
typedef int platform_posix_unused;
#endif
