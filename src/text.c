#include <stdint.h>

#include "text.h"

/* Lowercase form of a code point from U+0080..U+07FF (two-byte UTF-8);
 * every result is also in that range. */
static uint32_t fold_cp(uint32_t c)
{
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7)
        return c + 0x20;
    if (c >= 0x100 && c <= 0x12F)
        return c | 1;
    if (c >= 0x132 && c <= 0x137) /* U+0130/0131 (Turkish I) are left alone */
        return c | 1;
    if (c >= 0x139 && c <= 0x148)
        return (c & 1) ? c + 1 : c;
    if (c >= 0x14A && c <= 0x177)
        return c | 1;
    if (c == 0x178)
        return 0xFF;
    if (c >= 0x179 && c <= 0x17E)
        return (c & 1) ? c + 1 : c;
    if (c == 0x386)
        return 0x3AC;
    if (c >= 0x388 && c <= 0x38A)
        return c + 0x25;
    if (c == 0x38C)
        return 0x3CC;
    if (c == 0x38E || c == 0x38F)
        return c + 0x3F;
    if (c >= 0x391 && c <= 0x3AB && c != 0x3A2)
        return c + 0x20;
    if (c >= 0x400 && c <= 0x40F)
        return c + 0x50;
    if (c >= 0x410 && c <= 0x42F)
        return c + 0x20;
    return c;
}

void text_fold(char *s)
{
    unsigned char *p = (unsigned char *)s;

    while (*p) {
        if (*p >= 'A' && *p <= 'Z') {
            *p++ += 'a' - 'A';
        } else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            uint32_t c = fold_cp(((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F));
            p[0] = (unsigned char)(0xC0 | (c >> 6));
            p[1] = (unsigned char)(0x80 | (c & 0x3F));
            p += 2;
        } else {
            p++;
        }
    }
}
