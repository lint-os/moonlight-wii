/*
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "tls_rng.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __WII__
#include <network.h>
#include <ogc/es.h>
#include <ogc/lwp_watchdog.h>
#endif

// CTR_DRBG seed function. On the Wii it bypasses the prebuilt mbedtls
// platform entropy poll (which depends on ES/net being up) and mixes the
// 64-bit timebase, wall clock, device ID and MAC address instead.
int tls_rng(void *p_rng, unsigned char *output, size_t len)
{
    (void)p_rng;

#ifdef __WII__
    u32 devID = 0;
    u8 mac[6];

    if (ES_GetDeviceID(&devID) < 0)
        devID = 0;
    if (net_get_mac_address(mac) < 0)
        memset(mac, 0, sizeof(mac));

    srand((unsigned int)(gettime() ^ (u64)time(NULL)));

    size_t i = 0;
    while (i < len) {
        u64 tb = gettime();
        u32 w = (u32)tb ^ (u32)(tb >> 32) ^ (u32)time(NULL) ^
                (u32)rand() ^ devID ^ (u32)mac[(i / 4) % 6];
        size_t n = len - i < 4 ? len - i : 4;
        memcpy(output + i, &w, n);
        i += 4;
    }
    return 0;
#else
    srand((unsigned int)time(NULL));
    size_t i = 0;
    while (i < len) {
        unsigned int w = ((unsigned int)rand() << 16) | (unsigned int)rand();
        size_t n = len - i < 4 ? len - i : 4;
        memcpy(output + i, &w, n);
        i += 4;
    }
    return 0;
#endif
}
