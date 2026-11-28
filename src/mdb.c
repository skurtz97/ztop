/** @file mdb.c
 *  @brief The MDB object walk.  See src/mdb.h.
 */
#include "mdb.h"

#include <string.h>

/** Copy @p n bytes, stop at the first blank or NUL, terminate. */
static void trim_copy(char *dst, size_t len, const char *src, int n) {
    int i = 0;

    if (len == 0)
        return;
    while (i < n && src[i] != '\0' && src[i] != ' ')
        i++;
    if ((size_t)i >= len)
        i = (int)len - 1;
    (void)memcpy(dst, src, (size_t)i);
    dst[i] = '\0';
}

bool mdb_line(const char *mdb, int len, char *job, size_t joblen, char *text,
              size_t textlen) {
    const unsigned char *m = (const unsigned char *)mdb;
    int p;
    int found = 0;

    if (joblen > 0)
        job[0] = '\0';
    if (textlen > 0)
        text[0] = '\0';
    if (mdb == nullptr || len <= 0)
        return false;
    /* The header is 12 bytes; the objects follow it. */
    for (p = 12; p + 4 <= len;) {
        int olen = (m[p] << 8) | m[p + 1];
        int otype = (m[p + 2] << 8) | m[p + 3];

        if (olen < 4 || p + olen > len)
            break; /* a length that does not fit is not a length */
        if (otype == 1 && olen >= 56) {
            trim_copy(job, joblen, (const char *)m + p + 48, 8);
        } else if (otype == 4 && olen > 9 && found == 0) {
            trim_copy(text, textlen, (const char *)m + p + 9, olen - 9);
            found = 1;
        }
        p += olen;
    }
    return found != 0;
}
