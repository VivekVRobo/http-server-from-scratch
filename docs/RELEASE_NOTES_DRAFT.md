# Draft Release Notes — v0.1.0

## v0.1.0 — C++20 HTTP/1.1 Server Engineering Reference

This is the first software-reference release of `vhttp`, a cross-platform C++20 HTTP/1.1 server built from raw sockets so protocol behavior, connection lifecycle, secure static serving, concurrency and runtime scheduling remain explicit and testable.

### What this release demonstrates

- incremental HTTP/1.0 and HTTP/1.1 parsing;
- explicit message framing and chunked transfer decoding;
- routing, persistence, pipelining and response semantics;
- hardened static-file handling with traversal/encoding/range/validator tests;
- bounded blocking thread-pool runtime;
- Linux nonblocking `epoll` runtime sharing the same HTTP connection state machine;
- real loopback TCP and adversarial tests;
- GCC/Clang Linux verification and MSVC Windows verification;
- reproducible threadpool-vs-epoll benchmark campaign tooling with raw evidence preservation and paired analysis.

### Evidence boundary

This is an **engineering/educational server**, not a production-ready internet-facing reverse proxy.

CI benchmark smoke tests prove that the benchmark machinery executes; they are not controlled performance evidence. No general threadpool-vs-epoll winner or universal throughput/latency claim is made in this release.

### Controlled-host benchmark gate

Measured comparative claims remain blocked until M6B.2 is run on a documented Linux/WSL2 host with:

- exact Git revision and Release build;
- CPU/RAM/kernel/compiler provenance;
- repeated alternating runtime order;
- keep-alive and connection-churn matrices;
- raw server/client JSON and logs;
- p50/p95/p99 successful-request latency, throughput and failures;
- CPU and peak RSS;
- load-generator bottleneck review;
- scenario-scoped conclusions.

### Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Important current limitations

Handlers remain synchronous in the `epoll` event-loop thread; the blocking runtime assigns one worker to a slow/persistent connection; some payloads remain in-memory; the current static-file confinement is not race-free against hostile concurrent local filesystem mutation; no Windows IOCP backend exists yet.

### Release gate

Before publishing the GitHub Release, verify `docs/RELEASE_READINESS.md` against the exact tag commit. M6B.2 evidence is not required for the software-reference tag, but it is required for comparative performance claims.

License: MIT.
