/*
 * Copyright (c) 2026 Con Kolivas
 *
 * C interface to the Bitcoin Core mining IPC (Cap'n Proto) interface.
 *
 * The Cap'n Proto / C++ complexity lives entirely in mining_ipc.cpp so the
 * rest of ckpool stays pure C. All functions are exception safe: no C++
 * exception ever crosses this boundary, failures are reported as return
 * codes.
 *
 * Phase 1 exposes only the connection and chain-tip notification surface
 * (Mining.getTip / Mining.waitTipChanged) used to drive block updates. Block
 * template generation will be added in a later phase.
 */

#ifndef MINING_IPC_H
#define MINING_IPC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque connection context. */
typedef struct mining_ipc_ctx mining_ipc_ctx;

/* A chain tip: 64 char hex block hash (internal byte order, NUL terminated)
 * and its height. */
typedef struct {
	char hash[65];
	int32_t height;
} mining_tip;

/* Connect to the bitcoind mining IPC socket at socket_path (e.g.
 * "/path/to/node.sock"). Performs the full handshake and attempts to obtain a
 * Mining capability. Returns a context on success, or NULL if the socket is
 * absent or the connection could not be established. The returned context must
 * be released with mining_ipc_disconnect(). */
mining_ipc_ctx *mining_ipc_connect(const char *socket_path);

/* Tear down and free a context. Safe to call with NULL. */
void mining_ipc_disconnect(mining_ipc_ctx *ctx);

/* Re-obtain (or reconnect and re-obtain) a usable Mining capability. Called on
 * the recovery path when a wait or query fails. Returns 0 once a usable Mining
 * client is available, non-zero while bitcoind remains unavailable. */
int mining_ipc_try_obtain_mining(mining_ipc_ctx *ctx);

/* Fetch the current chain tip. Returns 0 and fills out_tip on success, non-zero
 * on failure (including a lost connection, which flags the context for
 * reconnection on the next mining_ipc_try_obtain_mining()). */
int mining_ipc_get_tip(mining_ipc_ctx *ctx, mining_tip *out_tip);

/* Block until the chain tip differs from current_tip_hex (64 hex chars) or
 * timeout_seconds elapses. Returns 0 and fills out_new_tip when a new tip
 * arrives; non-zero on timeout or failure. A timeout is a normal, non-fatal
 * result; a disconnection flags the context for reconnection. */
int mining_ipc_wait_tip_changed(mining_ipc_ctx *ctx,
				const char *current_tip_hex,
				double timeout_seconds,
				mining_tip *out_new_tip);

/* Interrupt a pending mining_ipc_wait_tip_changed() so it returns promptly. */
void mining_ipc_interrupt(mining_ipc_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* MINING_IPC_H */
