/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GSW_UPDATE_TRUST_H
#define GSW_UPDATE_TRUST_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GswTrustBuffer {
    unsigned char *data;
    size_t length;
} GswTrustBuffer;

/* The C++ helper owns its installation-wide .publish.lock for the complete
 * request. Returned bytes belong to this library and must be freed once.
 * Calls are synchronous. A cancel callback must not throw and its context must
 * remain alive until this function returns; it is invoked on this call's thread.
 * Re-entrant/concurrent in-process trust requests return TRUST_BUSY. */
GswTrustBuffer gsw_update_trust_request(const unsigned char *json, size_t length,
                                       int (*cancel)(void *), void *context);
void gsw_update_trust_free(GswTrustBuffer buffer);

#ifdef __cplusplus
}
#endif
#endif

