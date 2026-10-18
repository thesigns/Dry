/* diary.h – diary model: entries (one per day) → segments (one per /start) → paragraphs. */
#ifndef DRY_DIARY_H
#define DRY_DIARY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define DATE_LEN 10 /* "YYYY-MM-DD" */
#define TIME_LEN 5  /* "HH:MM" */

typedef struct {
    char time[TIME_LEN + 1];
    char **paras;
    size_t count, cap;
} Segment;

typedef struct {
    uint32_t id;
    char date[DATE_LEN + 1];
    Segment *segs;
    size_t count, cap;
} Entry;

typedef struct {
    Entry *entries;
    size_t count, cap;
} Diary;

void diary_init(Diary *d);
void diary_free(Diary *d);

/* Conversion to bytes and back (this is what gets encrypted). */
bool diary_serialize(const Diary *d, unsigned char **out, size_t *out_len);
bool diary_deserialize(Diary *d, const unsigned char *buf, size_t len);

/* Writes the whole diary as Markdown (UTF-8). Returns false on a write error. */
bool diary_write_markdown(const Diary *d, const char *title, FILE *f);

uint32_t diary_next_id(const Diary *d);
Entry *diary_find_id(Diary *d, uint32_t id);
Entry *diary_find_date(Diary *d, const char *date);

/* Note: adding an entry/segment may move the array in memory,
 * so previously obtained Entry* / Segment* pointers become invalid. */
Entry *diary_add_entry(Diary *d, const char *date);
Segment *entry_add_segment(Entry *e, const char *time);
bool segment_add_paragraph(Segment *s, const char *text);

/* Removes the entry's last paragraph; empty segments and an empty entry are removed too.
 * Returns the removed text (caller frees it) or NULL if there was nothing to remove. */
char *diary_undo_paragraph(Diary *d, Entry *e, bool *segment_removed, bool *entry_removed);

#endif
