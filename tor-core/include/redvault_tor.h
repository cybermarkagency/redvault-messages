// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

/*
 * Redvault's embedded Tor gateway (proof of concept): the C ABI.
 *
 * rv_tor_start runs Arti inside the process and opens a loopback SOCKS5 port
 * that requires the returned username/password and only connects to the onion
 * targets in the config. Point libcurl at it with
 *   CURLOPT_PROXY = "socks5h://127.0.0.1:<port>"
 *   CURLOPT_PROXYUSERNAME / CURLOPT_PROXYPASSWORD
 * ("socks5h" so libcurl never resolves the host itself). Every string returned
 * here is UTF-8 JSON, NUL-terminated; free it with rv_tor_free_string.
 */
#ifndef REDVAULT_TOR_H
#define REDVAULT_TOR_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RvTorGateway RvTorGateway;

/* config_json:
 *   {"stateDir":"…","cacheDir":"…","allowedTargets":["<v3>.onion:80"],
 *    "log":"warn"            (optional: Arti's log on stderr, addresses redacted)
 *    "testBlockedNetwork":false (test builds only)}
 * Returns NULL on failure, with a message in *error when error is not NULL.
 * Returns as soon as the SOCKS port is open; Tor bootstraps in the background. */
RvTorGateway* rv_tor_start(const char* config_json, char** error);

/* {"host":"127.0.0.1","port":N,"username":"…","password":"…"} */
char* rv_tor_endpoint(const RvTorGateway* gateway);

/* {"state":"connecting"|"connected"|"unavailable","progress":0-100,"detail":"…",
 *  "onionReachable":true|false|null,"stats":{…}} */
char* rv_tor_status(const RvTorGateway* gateway);

/* Non-zero pauses Tor's background work until the next request. */
void rv_tor_set_dormant(const RvTorGateway* gateway, int dormant);

/* Closes the SOCKS port and every stream, stops Tor, frees the gateway. */
void rv_tor_stop(RvTorGateway* gateway);

void rv_tor_free_string(char* text);

#ifdef __cplusplus
}
#endif

#endif /* REDVAULT_TOR_H */
