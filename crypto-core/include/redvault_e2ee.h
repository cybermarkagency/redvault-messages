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

/* Wipes and frees a response from rv_e2ee_call. */
void rv_e2ee_free(uint8_t* response, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* REDVAULT_E2EE_H */
