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
 * request. Returned bytes belong to this library and must be freed once. */
GswTrustBuffer gsw_update_trust_request(const unsigned char *json, size_t length,
                                       int (*cancel)(void *), void *context);
void gsw_update_trust_free(GswTrustBuffer buffer);

#ifdef __cplusplus
}
#endif
#endif

