/* dry – encrypted console diary.
 * Usage: dry <name>  →  opens or creates the file <name>.dry next to the program. */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <sodium.h>

#include "diary.h"
#include "platform.h"
#include "store.h"
#include "text.h"

#define PASSWORD_MAX  256
#define MAX_ATTEMPTS  3
#define LIST_DEFAULT  10
#define PREVIEW_CHARS 50
#define FIND_CONTEXT  30 /* characters shown on each side of a /find match */

typedef struct {
    Diary diary;
    Store store;
    bool writing;                  /* between /start and /end */
    char write_date[DATE_LEN + 1]; /* the day being written to */
    bool segment_open;             /* whether this writing session has created a segment */
    size_t written;                /* paragraphs added in this writing session */
} App;

/* --- output --- */

static void put(const char *s)
{
    platform_write(s, strlen(s));
}

static void out(const char *fmt, ...)
{
    char small[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n < sizeof small) {
        platform_write(small, (size_t)n);
    } else {
        char *big = malloc((size_t)n + 1);
        if (!big)
            return;
        va_start(ap, fmt);
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        platform_write(big, (size_t)n);
        free(big);
    }
}

static void free_line(char *line)
{
    if (line) {
        sodium_memzero(line, strlen(line));
        free(line);
    }
}

/* --- helpers --- */

static void current_date(char date[DATE_LEN + 1])
{
    struct tm tm;
    platform_localtime(time(NULL), &tm);
    strftime(date, DATE_LEN + 1, "%Y-%m-%d", &tm);
}

static void current_time(char t[TIME_LEN + 1])
{
    struct tm tm;
    platform_localtime(time(NULL), &tm);
    strftime(t, TIME_LEN + 1, "%H:%M", &tm);
}

static void rtrim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
        s[--n] = '\0';
}

static bool is_blank(const char *s)
{
    return s[strspn(s, " \t")] == '\0';
}

/* "kuba" → "<program dir>/kuba.dry"; paths and names with an extension are used as is. */
static char *resolve_path(const char *arg)
{
    const char *base = arg, *p;
    bool has_dir = false, has_ext;
    char *dir = NULL, *res;
    size_t len;

    for (p = arg; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') {
            has_dir = true;
            base = p + 1;
        }
    }
    has_ext = strchr(base, '.') != NULL;
    if (!has_dir) {
        dir = platform_exe_dir();
        if (!dir)
            return NULL;
    }
    len = (dir ? strlen(dir) + 1 : 0) + strlen(arg) + 5;
    res = malloc(len);
    if (res) {
        if (dir)
            snprintf(res, len, "%s%c%s%s", dir, PATH_SEP, arg, has_ext ? "" : ".dry");
        else
            snprintf(res, len, "%s%s", arg, has_ext ? "" : ".dry");
    }
    free(dir);
    return res;
}

/* Locks "<path>.lock" so that the diary cannot be open in two processes at once
 * (every save rewrites the whole file, so the last one would silently win). */
static bool lock_diary(const char *path, PlatformLock **lock)
{
    size_t len = strlen(path) + 6;
    char *lock_path = malloc(len);
    int r;

    if (!lock_path) {
        put("Out of memory.\n");
        return false;
    }
    snprintf(lock_path, len, "%s.lock", path);
    r = platform_lock(lock_path, lock);
    free(lock_path);
    if (r == LOCK_BUSY)
        put("The diary is already open in another window. Close it there first.\n");
    else if (r != LOCK_OK)
        out("Cannot create the lock file \"%s.lock\".\n", path);
    return r == LOCK_OK;
}

static bool save_diary(App *app)
{
    unsigned char *buf;
    size_t len;
    StoreResult r;

    if (!diary_serialize(&app->diary, &buf, &len)) {
        put("ERROR: out of memory – not saved!\n");
        return false;
    }
    r = store_save(&app->store, buf, len);
    sodium_memzero(buf, len);
    free(buf);
    if (r != STORE_OK) {
        out("ERROR: save failed (%s) – changes exist only in memory!\n", store_strerror(r));
        return false;
    }
    return true;
}

/* First words of an entry, cut at a word boundary (counts UTF-8 characters, not bytes). */
static void make_preview(const Entry *e, char *buf, size_t size)
{
    size_t pos = 0, chars = 0, last_space = 0;
    bool truncated = false;

    (void)size; /* PREVIEW_CHARS characters of up to 4 bytes + " (...)" fit in the buffer */
    for (size_t i = 0; i < e->count && !truncated; i++) {
        const Segment *s = &e->segs[i];
        for (size_t j = 0; j < s->count && !truncated; j++) {
            const unsigned char *p = (const unsigned char *)s->paras[j];
            if (pos > 0) {
                if (chars == PREVIEW_CHARS) {
                    truncated = true;
                    break;
                }
                chars++;
                last_space = pos;
                buf[pos++] = ' ';
            }
            for (; *p; p++) {
                if ((*p & 0xC0) != 0x80) { /* start of a character */
                    if (chars == PREVIEW_CHARS) {
                        truncated = true;
                        break;
                    }
                    chars++;
                }
                if (*p == ' ')
                    last_space = pos;
                buf[pos++] = (char)*p;
            }
        }
    }
    if (truncated && last_space > pos / 2)
        pos = last_space;
    while (pos > 0 && buf[pos - 1] == ' ')
        pos--;
    buf[pos] = '\0';
    if (truncated)
        strcat(buf, " (...)");
}

static void print_entry(const Entry *e)
{
    out("#%05u %s\n", (unsigned)e->id, e->date);
    for (size_t i = 0; i < e->count; i++) {
        const Segment *s = &e->segs[i];
        out("\n[%s]\n", s->time);
        for (size_t j = 0; j < s->count; j++) {
            put(s->paras[j]);
            put("\n");
        }
    }
}

static bool ask_yes(void)
{
    char *line = NULL;
    bool yes;

    if (platform_read_line(&line) != READ_OK)
        return false;
    rtrim(line);
    yes = strcmp(line, "y") == 0 || strcmp(line, "Y") == 0 || strcmp(line, "yes") == 0;
    free_line(line);
    return yes;
}

/* Asks for a new password twice into pw (both buffers PASSWORD_MAX, sodium_malloc). */
static bool ask_new_password(char *pw, char *pw2)
{
    for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
        bool match;

        put("New password: ");
        if (platform_read_password(pw, PASSWORD_MAX) != READ_OK)
            return false;
        if (pw[0] == '\0') {
            put("The password cannot be empty.\n");
            continue;
        }
        put("Repeat password: ");
        if (platform_read_password(pw2, PASSWORD_MAX) != READ_OK)
            return false;
        match = strcmp(pw, pw2) == 0;
        sodium_memzero(pw2, PASSWORD_MAX);
        if (match)
            return true;
        put("The passwords do not match.\n");
    }
    return false;
}

/* --- commands --- */

static void cmd_help(void)
{
    put("Commands:\n"
        "  /start      start (or continue) today's entry\n"
        "  /end        stop writing\n"
        "  /undo       remove the last paragraph of today's entry\n"
        "  /list [N]   last N entries (default 10)\n"
        "  /show [N]   full entry number N, date YYYY-MM-DD, or today's entry\n"
        "  /find TEXT  days that mention TEXT (case-insensitive)\n"
        "  /export [F] save the diary as an UNENCRYPTED Markdown file\n"
        "  /passwd     change the password\n"
        "  /help       this help\n"
        "  /exit       quit\n"
        "While writing, each Enter ends and saves a paragraph.\n"
        "To start a paragraph with \"/\", type \"//\".\n");
}

static void cmd_start(App *app)
{
    Entry *e;

    if (app->writing) {
        out("You are already writing the entry for %s. Finish it with /end.\n", app->write_date);
        return;
    }
    current_date(app->write_date);
    app->writing = true;
    app->segment_open = false;
    app->written = 0;
    e = diary_find_date(&app->diary, app->write_date);
    if (e)
        out("Appending to entry #%05u from %s.", (unsigned)e->id, app->write_date);
    else
        out("New entry #%05u for %s.", (unsigned)diary_next_id(&app->diary), app->write_date);
    put(" Enter ends a paragraph, /end stops writing.\n");
}

static void cmd_end(App *app)
{
    if (!app->writing) {
        put("You are not writing an entry right now.\n");
        return;
    }
    app->writing = false;
    if (app->written == 0)
        put("Done – nothing was added.\n");
    else
        out("Done – paragraphs added: %zu.\n", app->written);
}

static void add_paragraph(App *app, const char *text)
{
    char now[TIME_LEN + 1];
    Entry *e = diary_find_date(&app->diary, app->write_date);
    Segment *s;

    if (!e && !(e = diary_add_entry(&app->diary, app->write_date)))
        goto oom;
    if (!app->segment_open) {
        current_time(now);
        if (!entry_add_segment(e, now))
            goto oom;
        app->segment_open = true;
    }
    s = &e->segs[e->count - 1];
    if (!segment_add_paragraph(s, text))
        goto oom;
    app->written++;
    save_diary(app);
    return;
oom:
    put("ERROR: out of memory – the paragraph was not added.\n");
}

static void cmd_list(App *app, const char *arg)
{
    unsigned long n = LIST_DEFAULT;
    char preview[PREVIEW_CHARS * 4 + 16];
    size_t from;

    if (*arg) {
        char *end;
        n = strtoul(arg, &end, 10);
        if (*end || n == 0) {
            put("Usage: /list [N], e.g. /list 30\n");
            return;
        }
    }
    if (app->diary.count == 0) {
        put("The diary is empty. Start writing with /start.\n");
        return;
    }
    from = app->diary.count > n ? app->diary.count - n : 0;
    for (size_t i = from; i < app->diary.count; i++) {
        const Entry *e = &app->diary.entries[i];
        make_preview(e, preview, sizeof preview);
        out("#%05u %s -- %s\n", (unsigned)e->id, e->date, preview);
    }
}

static void cmd_show(App *app, const char *arg)
{
    Entry *e;

    if (!*arg) {
        char today[DATE_LEN + 1];
        current_date(today);
        e = diary_find_date(&app->diary, app->writing ? app->write_date : today);
        if (!e) {
            put("There is no entry for today yet.\n");
            return;
        }
    } else if (strlen(arg) == DATE_LEN && arg[4] == '-' && arg[7] == '-') {
        e = diary_find_date(&app->diary, arg);
        if (!e) {
            out("No entry for %s.\n", arg);
            return;
        }
    } else {
        char *end;
        unsigned long id = strtoul(arg[0] == '#' ? arg + 1 : arg, &end, 10);
        if (*end || id == 0 || id > UINT32_MAX) {
            put("Usage: /show [N | YYYY-MM-DD], e.g. /show 2\n");
            return;
        }
        e = diary_find_id(&app->diary, (uint32_t)id);
        if (!e) {
            out("No entry #%05lu.\n", id);
            return;
        }
    }
    print_entry(e);
}

static void cmd_undo(App *app)
{
    char today[DATE_LEN + 1], preview[PREVIEW_CHARS * 4 + 16];
    bool seg_removed, entry_removed;
    Entry *e, tmp;
    Segment seg;
    char *text;

    current_date(today);
    e = diary_find_date(&app->diary, app->writing ? app->write_date : today);
    if (!e) {
        put("Nothing to undo – only paragraphs of today's entry can be undone.\n");
        return;
    }
    text = diary_undo_paragraph(&app->diary, e, &seg_removed, &entry_removed);
    if (!text) {
        put("Nothing to undo.\n");
        return;
    }
    if (seg_removed)
        app->segment_open = false;
    if (app->written > 0)
        app->written--;

    /* Preview of the removed paragraph, using the same routine as /list. */
    memset(&tmp, 0, sizeof tmp);
    memset(&seg, 0, sizeof seg);
    seg.paras = &text;
    seg.count = 1;
    tmp.segs = &seg;
    tmp.count = 1;
    make_preview(&tmp, preview, sizeof preview);
    out("Removed paragraph: \"%s\"\n", preview);
    sodium_memzero(preview, sizeof preview);
    free_line(text);
    if (entry_removed)
        put("Today's entry is now empty, so it was removed.\n");
    save_diary(app);
}

/* File name part of a path. */
static const char *path_base(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':')
            base = p + 1;
    }
    return base;
}

/* Export path: no argument → "<diary>.md" next to the diary; a bare name goes into
 * the diary's directory; a path is used as given. ".md" is added if there is no extension. */
static char *export_path(const char *diary_path, const char *arg)
{
    const char *dir_end = path_base(diary_path);
    const char *name, *dot;
    size_t dir_len, name_len, len;
    char *res;

    if (*arg) {
        name = arg;
        dir_len = path_base(arg) != arg ? 0 : (size_t)(dir_end - diary_path);
        name_len = strlen(arg);
        dot = strchr(path_base(arg), '.');
    } else {
        name = dir_end;
        dir_len = (size_t)(dir_end - diary_path);
        dot = strrchr(dir_end, '.');
        name_len = dot ? (size_t)(dot - dir_end) : strlen(dir_end);
        dot = NULL;
    }
    len = dir_len + name_len + 4;
    res = malloc(len);
    if (res)
        snprintf(res, len, "%.*s%.*s%s", (int)dir_len, diary_path, (int)name_len, name,
                 dot ? "" : ".md");
    return res;
}

static void cmd_export(App *app, const char *arg)
{
    const char *base = path_base(app->store.path), *ext;
    char *path = export_path(app->store.path, arg), *title = NULL;
    size_t title_len;
    FILE *f;
    bool ok;

    if (!path) {
        put("ERROR: out of memory.\n");
        return;
    }
    ext = strrchr(path_base(path), '.');
    if (ext && strlen(ext) == 4 && (ext[1] | 0x20) == 'd' && (ext[2] | 0x20) == 'r' &&
        (ext[3] | 0x20) == 'y') {
        put("Refusing to export to a .dry file – pick another name, e.g. /export notes.md\n");
        goto done;
    }
    out("WARNING: the export is NOT encrypted. Anyone who can read\n"
        "  %s\n"
        "can read your whole diary. Delete the file when you no longer need it.\n"
        "Export the diary (entries: %zu)? [y/N] ", path, app->diary.count);
    if (!ask_yes()) {
        put("Export cancelled.\n");
        goto done;
    }
    if (platform_file_exists(path)) {
        put("The file already exists. Overwrite it? [y/N] ");
        if (!ask_yes()) {
            put("Export cancelled.\n");
            goto done;
        }
    }

    title_len = strrchr(base, '.') ? (size_t)(strrchr(base, '.') - base) : strlen(base);
    title = malloc(title_len + 1);
    f = title ? platform_fopen(path, "wb") : NULL;
    if (!f) {
        out("ERROR: cannot write \"%s\".\n", path);
        goto done;
    }
    memcpy(title, base, title_len);
    title[title_len] = '\0';
    ok = diary_write_markdown(&app->diary, title, f);
    if (fclose(f) != 0)
        ok = false;
    if (ok) {
        out("Exported to \"%s\".\n", path);
    } else {
        platform_remove_file(path);
        out("ERROR: writing \"%s\" failed – nothing was exported.\n", path);
    }
done:
    free(title);
    free(path);
}

/* Moves pos back / forward by up to n UTF-8 characters. */
static size_t utf8_back(const char *s, size_t pos, size_t n)
{
    while (pos > 0 && n > 0) {
        pos--;
        if (((unsigned char)s[pos] & 0xC0) != 0x80)
            n--;
    }
    return pos;
}

static size_t utf8_forward(const char *s, size_t pos, size_t n)
{
    while (s[pos] && n > 0) {
        pos++;
        while (((unsigned char)s[pos] & 0xC0) == 0x80)
            pos++;
        n--;
    }
    return pos;
}

/* One line per matching day: the number of matches and the context of the first one. */
static void print_match(const Entry *e, size_t matches, const char *para, size_t off,
                        size_t len)
{
    size_t from = utf8_back(para, off, FIND_CONTEXT);
    size_t to = utf8_forward(para, off + len, FIND_CONTEXT);

    /* Cut at word boundaries where the context has a space to cut at. */
    if (from > 0) {
        for (size_t p = from; p < off; p++) {
            if (para[p] == ' ') {
                from = p + 1;
                break;
            }
        }
    }
    if (para[to]) {
        for (size_t p = to; p > off + len; p--) {
            if (para[p] == ' ') {
                to = p;
                break;
            }
        }
    }

    out("#%05u %s (matches: %zu) -- %s%.*s%s\n", (unsigned)e->id, e->date, matches,
        from > 0 ? "(...) " : "", (int)(to - from), para + from, para[to] ? " (...)" : "");
}

static void cmd_find(App *app, const char *arg)
{
    size_t needle_len = strlen(arg), cap = 0, entries = 0;
    char *needle, *buf = NULL;

    if (!*arg) {
        put("Usage: /find TEXT, e.g. /find holiday\n");
        return;
    }
    needle = malloc(needle_len + 1);
    if (!needle) {
        put("ERROR: out of memory.\n");
        return;
    }
    memcpy(needle, arg, needle_len + 1);
    text_fold(needle);

    for (size_t i = 0; i < app->diary.count; i++) {
        const Entry *e = &app->diary.entries[i];
        const char *first = NULL; /* paragraph of the first match */
        size_t first_off = 0, matches = 0;

        for (size_t j = 0; j < e->count; j++) {
            const Segment *s = &e->segs[j];
            for (size_t k = 0; k < s->count; k++) {
                const char *para = s->paras[k], *hit;
                size_t len = strlen(para);

                if (len < needle_len)
                    continue;
                /* Grow by allocating anew, so no plaintext is left behind by realloc. */
                if (len + 1 > cap) {
                    if (buf) {
                        sodium_memzero(buf, cap);
                        free(buf);
                    }
                    cap = len + 1 > 2 * cap ? len + 1 : 2 * cap;
                    buf = malloc(cap);
                    if (!buf) {
                        put("ERROR: out of memory.\n");
                        goto done;
                    }
                }
                memcpy(buf, para, len + 1);
                text_fold(buf);
                for (hit = strstr(buf, needle); hit; hit = strstr(hit + needle_len, needle)) {
                    if (!first) {
                        first = para;
                        first_off = (size_t)(hit - buf);
                    }
                    matches++;
                }
            }
        }
        if (first) {
            print_match(e, matches, first, first_off, needle_len);
            entries++;
        }
    }
    if (entries == 0)
        out("Nothing found for \"%s\".\n", arg);
    else
        out("Matching days: %zu. Use /show N to read one.\n", entries);
done:
    if (buf) {
        sodium_memzero(buf, cap);
        free(buf);
    }
    sodium_memzero(needle, needle_len);
    free(needle);
}

static void cmd_passwd(App *app)
{
    char *pw = sodium_malloc(PASSWORD_MAX), *pw2 = sodium_malloc(PASSWORD_MAX);
    unsigned char *buf;
    size_t len;
    StoreResult r;

    if (!pw || !pw2) {
        put("ERROR: out of memory.\n");
        goto done;
    }
    put("Current password: ");
    if (platform_read_password(pw, PASSWORD_MAX) != READ_OK)
        goto done;
    if (!store_check_password(&app->store, pw)) {
        put("Wrong password – the password was not changed.\n");
        goto done;
    }
    if (!ask_new_password(pw, pw2)) {
        put("The password was not changed.\n");
        goto done;
    }
    if (!diary_serialize(&app->diary, &buf, &len)) {
        put("ERROR: out of memory – the password was not changed.\n");
        goto done;
    }
    r = store_change_password(&app->store, pw, buf, len);
    sodium_memzero(buf, len);
    free(buf);
    if (r != STORE_OK)
        out("ERROR: %s – the password was not changed.\n", store_strerror(r));
    else
        put("Password changed.\n");
done:
    if (pw)
        sodium_free(pw);
    if (pw2)
        sodium_free(pw2);
}

/* Returns false when the program should exit. */
static bool handle_line(App *app, char *line)
{
    rtrim(line);
    if (line[0] == '/' && line[1] != '/') {
        char *cmd = line + 1;
        char *arg = cmd + strcspn(cmd, " \t");
        if (*arg) {
            *arg++ = '\0';
            arg += strspn(arg, " \t");
        }
        if (strcmp(cmd, "start") == 0)
            cmd_start(app);
        else if (strcmp(cmd, "end") == 0)
            cmd_end(app);
        else if (strcmp(cmd, "undo") == 0)
            cmd_undo(app);
        else if (strcmp(cmd, "list") == 0)
            cmd_list(app, arg);
        else if (strcmp(cmd, "show") == 0)
            cmd_show(app, arg);
        else if (strcmp(cmd, "find") == 0)
            cmd_find(app, arg);
        else if (strcmp(cmd, "export") == 0)
            cmd_export(app, arg);
        else if (strcmp(cmd, "passwd") == 0)
            cmd_passwd(app);
        else if (strcmp(cmd, "help") == 0)
            cmd_help();
        else if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0)
            return false;
        else
            out("Unknown command /%s – type /help.\n", cmd);
        return true;
    }
    if (is_blank(line))
        return true;
    if (!app->writing) {
        put("You are not writing an entry – begin with /start (help: /help).\n");
        return true;
    }
    add_paragraph(app, line[0] == '/' ? line + 1 : line);
    return true;
}

/* --- opening / creating --- */

static bool open_existing(App *app, const char *path)
{
    char *pw = sodium_malloc(PASSWORD_MAX);
    bool ok = false;

    if (!pw)
        return false;
    for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
        unsigned char *plain;
        size_t plain_len;
        StoreResult r;

        put("Password: ");
        if (platform_read_password(pw, PASSWORD_MAX) != READ_OK)
            break;
        r = store_open(&app->store, path, pw, &plain, &plain_len);
        sodium_memzero(pw, PASSWORD_MAX);
        if (r == STORE_OK) {
            ok = diary_deserialize(&app->diary, plain, plain_len);
            sodium_memzero(plain, plain_len);
            free(plain);
            if (!ok) {
                put("The file was decrypted, but its contents are damaged.\n");
                store_close(&app->store);
            }
            break;
        }
        if (r != STORE_ERR_AUTH) {
            out("Cannot open the diary: %s.\n", store_strerror(r));
            break;
        }
        put("Wrong password or damaged file.\n");
    }
    sodium_free(pw);
    return ok;
}

static bool create_new(App *app, const char *path)
{
    char *pw = sodium_malloc(PASSWORD_MAX), *pw2 = sodium_malloc(PASSWORD_MAX);
    bool ok = false;
    StoreResult r;

    if (!pw || !pw2)
        goto done;
    out("Diary \"%s\" does not exist.\nCreate a new one? [y/N] ", path);
    if (!ask_yes()) {
        put("Cancelled.\n");
        goto done;
    }
    if (!ask_new_password(pw, pw2))
        goto done;

    r = store_create(&app->store, path, pw);
    if (r != STORE_OK) {
        out("Error: %s.\n", store_strerror(r));
        store_close(&app->store);
        goto done;
    }
    if (!save_diary(app)) {
        store_close(&app->store);
        goto done;
    }
    put("New diary created. Remember the password – without it your notes cannot be read.\n");
    ok = true;
done:
    if (pw)
        sodium_free(pw);
    if (pw2)
        sodium_free(pw2);
    return ok;
}

static void run(App *app)
{
    for (;;) {
        char *line = NULL;
        bool go_on;

        if (app->writing)
            out("%s> ", app->write_date);
        else
            put("dry> ");
        if (platform_read_line(&line) != READ_OK) {
            put("\n");
            break;
        }
        go_on = handle_line(app, line);
        free_line(line);
        if (!go_on)
            break;
    }
}

int main(int argc, char **argv)
{
    App app;
    PlatformLock *lock = NULL;
    char **uargv;
    char *path;
    int uargc = 0;
    bool ok;

    platform_init();
    if (sodium_init() < 0) {
        put("Failed to initialize the libsodium library.\n");
        return 1;
    }
    uargv = platform_utf8_argv(argc, argv, &uargc);
    if (!uargv || uargc != 2 || uargv[1][0] == '-') {
        put("dry – encrypted diary\n"
            "Usage: dry <name>\n"
            "  e.g. dry kuba  – opens or creates the file kuba.dry next to the program\n");
        return 2;
    }
    path = resolve_path(uargv[1]);
    if (!path) {
        put("Out of memory.\n");
        return 1;
    }

    if (!lock_diary(path, &lock)) {
        free(path);
        return 1;
    }

    memset(&app, 0, sizeof app);
    diary_init(&app.diary);
    ok = platform_file_exists(path) ? open_existing(&app, path) : create_new(&app, path);
    if (ok) {
        out("Diary \"%s\" opened (entries: %zu). Help: /help\n", path, app.diary.count);
        run(&app);
        diary_free(&app.diary);
        store_close(&app.store);
        platform_clear_screen();
        put("Diary closed.\n");
    }
    platform_unlock(lock);
    free(path);
    return ok ? 0 : 1;
}
