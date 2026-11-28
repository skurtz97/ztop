/**
 * @file zmcs.c
 * @brief The portable half of the console reader: there is none.
 *
 * The console lives in src/zmcs.s, which is built only on z/OS.  The
 * tests and the portable build still link the symbol, so this file
 * answers "no" instead of letting the link fail.  It is deliberately
 * the only thing here: a fake console would let a change look tested.
 */
#include "zmcs.h"

#ifndef __MVS__

int zmcs(zmcs_req *req) {
    req->rc = 8;
    req->rsn = 0;
    if (req->len != nullptr)
        *req->len = 0;
    return 0;
}

#endif /* !__MVS__ */
