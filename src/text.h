/* text.h – UTF-8 text helpers. */
#ifndef DRY_TEXT_H
#define DRY_TEXT_H

/* Lowercases s in place for case-insensitive comparison. Covers ASCII, Latin-1,
 * Latin Extended-A (all Polish letters), Greek and Cyrillic. The byte length of
 * every character is preserved, so offsets in the folded text match the original. */
void text_fold(char *s);

#endif
