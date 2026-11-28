/**
 * @file test_mdb.c
 * @brief The message-block walk, against blocks built byte by byte.
 *
 * The shape is the one IEAVM105 documents: a 12-byte header, then objects
 * of two bytes of length and two of type.  A general object carries the
 * job name at +48 and a text object its line at +9.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mdb.h"

/** A general object: 56 bytes, job name at +48. */
static void put_general(unsigned char *p, const char *job) {
    p[0] = 0;
    p[1] = 56;
    p[2] = 0;
    p[3] = 1;
    (void)memset(p + 48, ' ', 8);
    (void)memcpy(p + 48, job, strlen(job));
}

/** A text object: the line starts at +9, so the length is 9 + the line. */
static void put_text(unsigned char *p, const char *line) {
    int n = (int)strlen(line);

    p[0] = (unsigned char)((9 + n) >> 8);
    p[1] = (unsigned char)(9 + n);
    p[2] = 0;
    p[3] = 4;
    (void)memcpy(p + 9, line, (size_t)n);
}

int main(void) {
    static unsigned char mdb[512];
    char job[16];
    char text[MDB_TEXT_MAX];

    /* Job and line, in the order the service writes them. */
    (void)memset(mdb, 0, sizeof mdb);
    put_general(mdb + 12, "JOB1");
    put_text(mdb + 12 + 56, "IEF403I JOB1 - STARTED");
    assert(mdb_line((const char *)mdb, 12 + 56 + 9 + 22, job, sizeof job, text,
                    sizeof text));
    assert(strcmp(job, "JOB1") == 0);
    assert(strcmp(text, "IEF403I") == 0); /* trimmed at the first blank */

    /* A line of one word keeps the word. */
    (void)memset(mdb, 0, sizeof mdb);
    put_general(mdb + 12, "J2");
    put_text(mdb + 12 + 56, "STARTED");
    assert(mdb_line((const char *)mdb, 12 + 56 + 9 + 7, job, sizeof job, text,
                    sizeof text));
    assert(strcmp(job, "J2") == 0 && strcmp(text, "STARTED") == 0);

    /* No text object: nothing to show, and the job is still known. */
    (void)memset(mdb, 0, sizeof mdb);
    put_general(mdb + 12, "J3");
    assert(!mdb_line((const char *)mdb, 12 + 56, job, sizeof job, text,
                     sizeof text));
    assert(strcmp(job, "J3") == 0 && text[0] == '\0');

    /* A length that runs past the copy stops the walk: the block comes
     * from another address space, so its lengths are input. */
    (void)memset(mdb, 0, sizeof mdb);
    put_text(mdb + 12, "SHOULD NOT APPEAR");
    mdb[12] = 0x7F; /* claims 32k */
    mdb[13] = 0xFF;
    assert(!mdb_line((const char *)mdb, 12 + 9 + 16, job, sizeof job, text,
                     sizeof text));

    /* Empty and short blocks are refused, not walked. */
    assert(!mdb_line(nullptr, 8, job, sizeof job, text, sizeof text));
    assert(!mdb_line((const char *)mdb, 8, job, sizeof job, text, sizeof text));
    assert(!mdb_line((const char *)mdb, 0, job, sizeof job, text, sizeof text));

    /* A zero-length object ends the walk without reading further. */
    (void)memset(mdb, 0, sizeof mdb);
    mdb[12] = 0;
    mdb[13] = 0;
    mdb[14] = 0;
    mdb[15] = 4;
    assert(
        !mdb_line((const char *)mdb, 32, job, sizeof job, text, sizeof text));

    (void)printf("test_mdb: ok\n");
    return 0;
}
