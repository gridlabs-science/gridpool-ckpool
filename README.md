# GridPool CKPool Fork

This repository tracks current upstream CKPool and carries an early optional
integration for GridPool-compatible Stratum V1 payout templates. It is a public
beta canary, not a replacement for the GridPool reference node.

Start with:

- [GridPool handbook](https://github.com/gridlabs-science/gridpool-handbook)
- [GridPool reference node](https://github.com/gridlabs-science/boot-protocol)
- [Fork-specific integration notes](GRIDPOOL.md)
- [GridPool CKPool adapter](https://github.com/gridlabs-science/gridpool-ckpool-adapter)
- [Upstream CKPool README](README)

The Rust adapter owns HTTP, retries, durable proof queues, and fee scheduling.
CKPool remains responsible for Stratum V1 jobs and direct local block
submission. The GridPool node remains the consensus validator.

## Build

```bash
./configure
make -j2
```

The optional mining IPC path requires Cap'n Proto `1.1.x`, matching the
checked-in generated bindings. Older distribution packages such as Cap'n Proto
`0.8` cannot compile that path. Use a matching toolchain before validating the
GridPool integration.

## License

This fork retains CKPool's original licensing. See [COPYING](COPYING) and the
individual source headers.
