/** @file mdb.h
 *  @brief Pull a job name and a line of text out of a message data block.
 *
 * An MDB (IEAVM105) is a chain of objects, each two bytes of length
 * followed by two of type.  The general object (type 1) names the job
 * that wrote the message at +48; a text object (type 4) holds its line at
 * +9.  The walk is here rather than in src/ztop.c because this is the one
 * place ztop parses bytes that came from another address space, and it is
 * the part worth a test that runs anywhere.
 */
#ifndef ZTOP_MDB_H
#define ZTOP_MDB_H

#include <stddef.h>

/** Largest text line kept: a console line is at most 132 columns. */
enum { MDB_TEXT_MAX = 200 };

/**
 * @brief Extract the job name and the first text object of an MDB copy.
 *
 * @param mdb    the block as zmcs() copied it out of the console's space
 * @param len    how many bytes of it are valid
 * @param job    out: job name, blank padded or empty; never left unterminated
 * @param joblen size of @p job
 * @param text   out: the line, trimmed; empty when there is none
 * @param textlen size of @p text
 * @return true when a text object was found
 *
 * Every length read is checked against @p len before it is used: the
 * block was written by someone else, so a length in it is input, not a
 * fact.
 */
bool mdb_line(const char *mdb, int len, char *job, size_t joblen, char *text,
              size_t textlen);

#endif /* ZTOP_MDB_H */
