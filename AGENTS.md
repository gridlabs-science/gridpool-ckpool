# GridPool CKPool Fork Agent Guide

This repository tracks current upstream CKPool and adds the smallest practical
GridPool-specific Stratum V1 integration.

- Preserve ordinary CKPool behavior for non-GridPool clients.
- Attribute GridPool work from the actual slot-0 output, never username data.
- Bind issued jobs to their original parent, payout plan, and fee decision.
- Never truncate a GridPool payout suffix.
- Submit solved blocks locally before adapter notification.
- Keep HTTP, retries, durable queues, and fee cryptography in the Rust sidecar.

Validate with `make check` and an end-to-end regtest stack before deployment.

