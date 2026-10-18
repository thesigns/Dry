#include <stdlib.h>
#include <string.h>

#include <sodium.h>

#include "diary.h"

/* Decrypted data format (u32 integers are little-endian):
 *   u8 version
 *   u32 entry count, for each entry:
 *     u32 id, 10 B date, u32 segment count, for each segment:
 *       5 B time, u32 paragraph count, for each paragraph:
 *         u32 length, UTF-8 text (no terminating NUL) */
enum { PAYLOAD_VERSION = 1 };

/* Minimum sizes – used to reject absurd counts in damaged data. */
enum { MIN_ENTRY = 4 + DATE_LEN + 4, MIN_SEGMENT = TIME_LEN + 4, MIN_PARA = 4 };

static void free_text(char *s)
{
    if (s) {
        sodium_memzero(s, strlen(s));
        free(s);
    }
}

static void segment_free(Segment *s)
{
    for (size_t i = 0; i < s->count; i++)
        free_text(s->paras[i]);
    free(s->paras);
}

static void entry_free(Entry *e)
{
    for (size_t i = 0; i < e->count; i++)
        segment_free(&e->segs[i]);
    free(e->segs);
}

/* Ensures room for one more element; returns the (possibly moved) array or NULL. */
static void *grow(void *arr, size_t *cap, size_t count, size_t elem)
{
    size_t ncap;
    void *p;

    if (count < *cap)
        return arr;
    ncap = *cap ? *cap * 2 : 4;
    if (ncap > SIZE_MAX / elem)
        return NULL;
    p = realloc(arr, ncap * elem);
    if (p)
        *cap = ncap;
    return p;
}

void diary_init(Diary *d)
{
    memset(d, 0, sizeof *d);
}

void diary_free(Diary *d)
{
    for (size_t i = 0; i < d->count; i++)
        entry_free(&d->entries[i]);
    free(d->entries);
    diary_init(d);
}

uint32_t diary_next_id(const Diary *d)
{
    uint32_t max = 0;
    for (size_t i = 0; i < d->count; i++)
        if (d->entries[i].id > max)
            max = d->entries[i].id;
    return max + 1;
}

Entry *diary_find_id(Diary *d, uint32_t id)
{
    for (size_t i = 0; i < d->count; i++)
        if (d->entries[i].id == id)
            return &d->entries[i];
    return NULL;
}

Entry *diary_find_date(Diary *d, const char *date)
{
    for (size_t i = d->count; i-- > 0;)
        if (strcmp(d->entries[i].date, date) == 0)
            return &d->entries[i];
    return NULL;
}

static Entry *add_entry(Diary *d, uint32_t id, const char *date)
{
    Entry *arr = grow(d->entries, &d->cap, d->count, sizeof *arr);
    Entry *e;

    if (!arr)
        return NULL;
    d->entries = arr;
    e = &d->entries[d->count++];
    memset(e, 0, sizeof *e);
    e->id = id;
    memcpy(e->date, date, DATE_LEN);
    e->date[DATE_LEN] = '\0';
    return e;
}

Entry *diary_add_entry(Diary *d, const char *date)
{
    return add_entry(d, diary_next_id(d), date);
}

Segment *entry_add_segment(Entry *e, const char *time)
{
    Segment *arr = grow(e->segs, &e->cap, e->count, sizeof *arr);
    Segment *s;

    if (!arr)
        return NULL;
    e->segs = arr;
    s = &e->segs[e->count++];
    memset(s, 0, sizeof *s);
    memcpy(s->time, time, TIME_LEN);
    s->time[TIME_LEN] = '\0';
    return s;
}

/* Adds a paragraph, taking ownership of the allocated text. */
static bool segment_push(Segment *s, char *text)
{
    char **arr = grow(s->paras, &s->cap, s->count, sizeof *arr);
    if (!arr)
        return false;
    s->paras = arr;
    s->paras[s->count++] = text;
    return true;
}

bool segment_add_paragraph(Segment *s, const char *text)
{
    size_t n = strlen(text) + 1;
    char *copy = malloc(n);

    if (!copy)
        return false;
    memcpy(copy, text, n);
    if (!segment_push(s, copy)) {
        free_text(copy);
        return false;
    }
    return true;
}

char *diary_undo_paragraph(Diary *d, Entry *e, bool *segment_removed, bool *entry_removed)
{
    char *text = NULL;
    Segment *s;

    *segment_removed = *entry_removed = false;
    while (e->count > 0 && e->segs[e->count - 1].count == 0) {
        segment_free(&e->segs[--e->count]);
        *segment_removed = true;
    }
    if (e->count > 0) {
        s = &e->segs[e->count - 1];
        text = s->paras[--s->count];
        if (s->count == 0) {
            segment_free(s);
            e->count--;
            *segment_removed = true;
        }
    }
    if (e->count == 0) {
        size_t idx = (size_t)(e - d->entries);
        entry_free(e);
        memmove(&d->entries[idx], &d->entries[idx + 1],
                (d->count - idx - 1) * sizeof *d->entries);
        d->count--;
        *entry_removed = true;
    }
    return text;
}

/* --- serialization --- */

static unsigned char *put_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
    return p + 4;
}

static unsigned char *put_bytes(unsigned char *p, const void *src, size_t n)
{
    memcpy(p, src, n);
    return p + n;
}

bool diary_serialize(const Diary *d, unsigned char **out, size_t *out_len)
{
    size_t size = 1 + 4;
    unsigned char *buf, *p;

    /* Compute the size first so the buffer is allocated once – realloc
     * would leave unzeroed copies of the text in memory. */
    for (size_t i = 0; i < d->count; i++) {
        const Entry *e = &d->entries[i];
        size += MIN_ENTRY;
        for (size_t j = 0; j < e->count; j++) {
            const Segment *s = &e->segs[j];
            size += MIN_SEGMENT;
            for (size_t k = 0; k < s->count; k++)
                size += MIN_PARA + strlen(s->paras[k]);
        }
    }

    buf = malloc(size);
    if (!buf)
        return false;
    p = buf;
    *p++ = PAYLOAD_VERSION;
    p = put_u32(p, (uint32_t)d->count);
    for (size_t i = 0; i < d->count; i++) {
        const Entry *e = &d->entries[i];
        p = put_u32(p, e->id);
        p = put_bytes(p, e->date, DATE_LEN);
        p = put_u32(p, (uint32_t)e->count);
        for (size_t j = 0; j < e->count; j++) {
            const Segment *s = &e->segs[j];
            p = put_bytes(p, s->time, TIME_LEN);
            p = put_u32(p, (uint32_t)s->count);
            for (size_t k = 0; k < s->count; k++) {
                size_t n = strlen(s->paras[k]);
                p = put_u32(p, (uint32_t)n);
                p = put_bytes(p, s->paras[k], n);
            }
        }
    }
    *out = buf;
    *out_len = size;
    return true;
}

typedef struct {
    const unsigned char *p;
    size_t left;
} Reader;

static bool get_bytes(Reader *r, void *dst, size_t n)
{
    if (r->left < n)
        return false;
    memcpy(dst, r->p, n);
    r->p += n;
    r->left -= n;
    return true;
}

static bool get_u32(Reader *r, uint32_t *v)
{
    unsigned char b[4];
    if (!get_bytes(r, b, 4))
        return false;
    *v = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return true;
}

bool diary_deserialize(Diary *d, const unsigned char *buf, size_t len)
{
    Reader r = { buf, len };
    unsigned char version;
    uint32_t n_entries;

    diary_init(d);
    if (!get_bytes(&r, &version, 1) || version != PAYLOAD_VERSION)
        return false;
    if (!get_u32(&r, &n_entries) || n_entries > r.left / MIN_ENTRY)
        goto fail;

    for (uint32_t i = 0; i < n_entries; i++) {
        uint32_t id, n_segs;
        char date[DATE_LEN + 1] = { 0 };
        Entry *e;

        if (!get_u32(&r, &id) || !get_bytes(&r, date, DATE_LEN))
            goto fail;
        if (!get_u32(&r, &n_segs) || n_segs > r.left / MIN_SEGMENT)
            goto fail;
        e = add_entry(d, id, date);
        if (!e)
            goto fail;

        for (uint32_t j = 0; j < n_segs; j++) {
            char time[TIME_LEN + 1] = { 0 };
            uint32_t n_paras;
            Segment *s;

            if (!get_bytes(&r, time, TIME_LEN))
                goto fail;
            if (!get_u32(&r, &n_paras) || n_paras > r.left / MIN_PARA)
                goto fail;
            s = entry_add_segment(e, time);
            if (!s)
                goto fail;

            for (uint32_t k = 0; k < n_paras; k++) {
                uint32_t n;
                char *text;

                if (!get_u32(&r, &n) || n > r.left)
                    goto fail;
                text = malloc((size_t)n + 1);
                if (!text)
                    goto fail;
                get_bytes(&r, text, n);
                text[n] = '\0';
                if (!segment_push(s, text)) {
                    free_text(text);
                    goto fail;
                }
            }
        }
    }
    if (r.left != 0)
        goto fail;
    return true;

fail:
    diary_free(d);
    return false;
}
