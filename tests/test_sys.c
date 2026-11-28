/**
 * @file test_sys.c
 * @brief The IPL decoder, against values read from a live SMCA.
 */
#include <assert.h>
#include <stdio.h>

#include "sys.h"

int main(void) {
    zipl z;

    /* CBT, IPLed at 17:02:47 on 21 September 2026 (day 264). */
    assert(zipl_decode(0x0126264Fu, 6136760u, &z));
    assert(z.year == 2026 && z.month == 9 && z.day == 21);
    assert(z.hour == 17 && z.minute == 2 && z.second == 47);

    /* 1926, day 260 (17 September), and 2024, day 260 (16 September):
     * the century nibble and the leap year both move the answer. */
    assert(zipl_decode(0x0026260Fu, 0u, &z));
    assert(z.year == 1926 && z.month == 9 && z.day == 17);
    assert(zipl_decode(0x0124260Fu, 0u, &z));
    assert(z.year == 2024 && z.month == 9 && z.day == 16);

    /* 23:59:59.99 is the last second of the day. */
    assert(zipl_decode(0x0126264Fu, 8639999u, &z));
    assert(z.hour == 23 && z.minute == 59 && z.second == 59);

    /* Out of range: century 2, day 0, day 366 in a common year, 24:00. */
    assert(!zipl_decode(0x0226264Fu, 0u, &z));
    assert(!zipl_decode(0x0126000Fu, 0u, &z));
    assert(!zipl_decode(0x0126366Fu, 0u, &z));
    assert(!zipl_decode(0x0126264Fu, 8640000u, &z));

    (void)printf("test_sys: ok\n");
    return 0;
}
