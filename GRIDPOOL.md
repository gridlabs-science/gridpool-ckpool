# GridPool CKPool Integration

Status: early public-beta integration fork based on current upstream CKPool.

This fork keeps CKPool's ordinary solo behavior and adds an optional GridPool
coinbase variant per Stratum V1 connection. It requires the local
`gridpool-ckpool-adapter` daemon and a GridPool reference node.

## Hosted Mode

Use `address.worker` as the SV1 username and include the exact token
`USE_GRIDPOOL_SPLIT` in the password. Tokens may be separated by commas or
semicolons and comparison is case-insensitive. Connections without the token
continue using CKPool's ordinary templates.

## Private Mode

Set `gridpool_default_enabled` to enable GridPool for every connection and set
`gridpool_fixed_address` when all workers should share one slot-0 address.

## Atlas Fee

Fee scheduling is configured in the adapter. During selected ten-second
buckets, the Atlas operator address replaces the user's address in slot 0. The
complete GridPool payout suffix remains unchanged, and resulting proofs remain
GridPool-valid. Because slot 0 controls both block-finder and proof attribution,
a 150 basis-point work schedule produces an approximately 1.5% long-run fee.

## Safety

- A payout plan is bound to the Bitcoin parent and active GridPool snapshot.
- Every retained CKPool workbase keeps its original GridPool coinbase variant.
- Missing or mismatched plans pause opted-in clients; they never silently fall
  back to solo templates.
- Solved blocks are submitted directly to the configured Bitcoin node before
  their proof is handed to the adapter.
- Ordinary accepted vardiff shares are sent through a separate asynchronous
  telemetry queue. Reserve-qualifying proofs always carry the full header,
  coinbase, and Merkle path to GridPool; lower pulse proofs are bounded to one
  per client per 30 seconds and must meet GridPool's advertised pulse floor.
- Coinbase allocation is dynamic and bounded by `gridpool_max_coinbase_bytes`.
  The GridPool suffix is never truncated.

GridPool's 300-slot templates can exceed firmware-specific SV1 coinbase limits.
Test firmware compatibility before moving material hashrate.
