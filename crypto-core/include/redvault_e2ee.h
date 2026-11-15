// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

/*
 * Redvault's shared end-to-end encryption core (proof of concept): the C ABI.
 * Every call takes one JSON request and returns one JSON response; the
 * commands are described in docs/vodozemac-poc.md. Thread-safe: the engine
 * keeps no state between calls (the caller stores the encrypted state each
 * successful response returns, before sending anything it produced).
 */
#ifndef REDVAULT_E2EE_H
#define REDVAULT_E2EE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Runs one UTF-8 JSON request of `len` bytes. Returns the UTF-8 JSON response
 * (not NUL-terminated), its length in *out_len. NULL only if out_len is NULL.
 * Free with rv_e2ee_free. */
uint8_t* rv_e2ee_call(const uint8_t* request, size_t len, size_t* out_len);

/* Wipes and frees a response from rv_e2ee_call (and the outputs of rv_sealed_*). */
void rv_e2ee_free(uint8_t* response, size_t len);

/*
 * Sealed requests for the backup route (docs/tor-sealed-fallback.md), built with
 * the sealed-transport feature: Oblivious HTTP (RFC 9458) to the gateway's pinned
 * key. Each returns 0 on success, -1 for bad input, -2 if sealing/opening failed.
 */

/* Seals one request. head_json: {"method":"POST","path":"/api/...","headers":[["name","value"],...]}.
 * On success *sealed/*sealed_len is the message/ohttp-req body to POST (free with rv_e2ee_free)
 * and *pending opens the reply: pass it to rv_sealed_open or rv_sealed_discard exactly once. */
int32_t rv_sealed_seal(const uint8_t* key_config, size_t key_config_len,
                       const uint8_t* head_json, size_t head_len,
                       const uint8_t* body, size_t body_len,
                       uint8_t** sealed, size_t* sealed_len, void** pending);

/* Opens a message/ohttp-res reply; always consumes `pending`. On success *head_json is
 * {"status":200,"headers":[["name","value"],...]} and *body the body (free both with rv_e2ee_free). */
int32_t rv_sealed_open(void* pending, const uint8_t* reply, size_t reply_len,
                       uint8_t** head_json, size_t* head_len, uint8_t** body, size_t* body_len);

/* Drops a pending handle whose reply never came. */
void rv_sealed_discard(void* pending);

#ifdef __cplusplus
}
#endif

#endif /* REDVAULT_E2EE_H */
