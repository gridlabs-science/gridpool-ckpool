/*
 * Minimal local IPC client for the GridPool CKPool adapter.
 * GridPool consensus and HTTP retry logic intentionally remain outside ckpool.
 */
#ifndef GRIDPOOL_ADAPTER_H
#define GRIDPOOL_ADAPTER_H

#include "libckpool.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GRIDPOOL_PLAN_ID_LEN 64
#define GRIDPOOL_SNAPSHOT_ID_MAX 128

typedef struct gridpool_plan {
	bool available;
	char plan_id[GRIDPOOL_PLAN_ID_LEN + 1];
	char snapshot_id[GRIDPOOL_SNAPSHOT_ID_MAX + 1];
	char parent_hash[65];
	unsigned char *suffix;
	size_t suffix_len;
	uint64_t suffix_value;
	uint32_t suffix_outputs;
	double minimum_reserve_difficulty;
	double minimum_pulse_difficulty;
} gridpool_plan_t;

void gridpool_plan_clear(gridpool_plan_t *plan);
bool gridpool_adapter_get_plan(const char *socket_path, size_t maximum_message_bytes,
			       gridpool_plan_t *plan);
bool gridpool_adapter_fee_decision(const char *socket_path, size_t maximum_message_bytes,
				   const char *parent_hash, const char *payout_script_hex,
				   int64_t unix_seconds, bool *fee_active, int64_t *bucket);
bool gridpool_adapter_submit_proof(const char *socket_path, size_t maximum_message_bytes,
				   const char *proof_json);
bool gridpool_adapter_record_share(const char *socket_path, size_t maximum_message_bytes,
				   const char *channel_id, const char *payout_address,
				   const char *username, bool accepted, double difficulty,
				   bool fee_work, int64_t observed_unix_ms);
bool gridpool_password_enabled(const char *password);

#endif /* GRIDPOOL_ADAPTER_H */
