# vhttp — HTTP/1.1 Server From Scratch

A cross-platform C++20 HTTP server built from raw sockets to make protocol parsing, routing, connection lifecycle, message framing, static-file security, bounded concurrency, event-driven I/O, and performance verification visible and testable.

> Status: **Milestone 6B event-runtime correctness implemented.** The repository now contains both a bounded blocking thread-pool runtime and a Linux nonblocking `epoll` runtime sharing the same HTTP connection state machine. Performance/scalability superiority is **not claimed yet**; comparative evidence remains gated on M6B.1.

This remains an educational/engineering server, not a production-ready internet-facing reverse proxy.

## Why this project exists

The important HTTP server layers are implemented here rather than delegated to an HTTP framework. OS and standard-library facilities are used, but TCP integration, incremental parsing, routing, framing, persistence, static-file semantics, runtime scheduling, backpressure, and verification remain explicit.

## Current capabilities

### Transport and shared HTTP connection lifecycle

- Windows Winsock2 and POSIX socket abstraction for the blocking runtime.
- RAII listeners/streams, TCP client connect support, and ephemeral-port discovery.
- Transport-agnostic `ConnectionSession` shared by blocking and event runtimes.
- HTTP/1.1 persistence by default; HTTP/1.0 keep-alive opt-in.
- Multiple requests per socket with preserved pipelined bytes.
- Pipelined requests dispatch in order and only after the previous response write is acknowledged complete.
- Configurable max requests per connection and idle timeout.
- Parser errors become one closing `400 Bad Request` response when the transport can still write it.

### HTTP parsing and framing

- Incremental HTTP/1.0 + HTTP/1.1 parsing across arbitrary TCP reads.
- Case-insensitive headers and bounded request/header/body parsing.
- `Content-Length` bodies.
- Incremental `Transfer-Encoding: chunked` decoding with bounded extensions/trailers and overflow/body-budget checks.
- Rejection of `Transfer-Encoding` + `Content-Length` ambiguity and unsupported coding chains.
- Chunked response encoding and serializer-authoritative message framing.

### Routing and response semantics

- Method-aware route trie with static and `:parameter` segments.
- Query/path-parameter access.
- Correct `404` / `405`, deterministic `Allow`, automatic `OPTIONS`, and `HEAD` fallback.
- Body-forbidden statuses (`1xx`, `204`, `304`) emit no payload framing/body.
- `HEAD` suppresses payload bytes while preserving GET-equivalent metadata.

### Secure static file engine

- URL-prefix-to-document-root mapping.
- Strict percent decoding and rejection of encoded/raw separators, backslashes, NUL/control bytes, and `.` / `..` traversal.
- Canonical candidate containment checks after symlink/reparse resolution.
- Explicit directory index policy and regular-file-only serving.
- Configurable maximum file size for the current in-memory GET path.
- MIME detection + `X-Content-Type-Options: nosniff`.
- Weak ETags / `If-None-Match` and `Last-Modified` / `If-Modified-Since`.
- Single closed/open-ended/suffix byte ranges with `206`, `416`, `Content-Range`, and date-based `If-Range` policy.
- `HEAD` resolves metadata/ranges without reading file payload bytes.

### Bounded thread-pool runtime

- Configurable fixed worker count.
- Bounded pending accepted-connection queue.
- Queue saturation closes and counts excess accepted transports instead of growing memory without bound.
- No detached worker/connection threads.
- `request_stop()` stops new acceptance and drains queued + active work before workers are joined.
- Per-connection exception containment.
- Runtime counters for accepted, rejected, completed, failed, active, and queued connections.
- Every worker reuses the same `Server::serve_connection()` + `ConnectionSession` HTTP semantics.

### Linux `epoll` event runtime

- Nonblocking listener and accepted sockets with CLOEXEC descriptors.
- Level-triggered `epoll` accept/read/write readiness.
- Bounded active-connection admission with rejection accounting.
- Partial nonblocking writes with offset-based resume.
- Read-side backpressure while a response is pending.
- At most one serialized response buffered per connection.
- Configurable `max_pending_output_bytes` guard.
- Idle connection retirement without busy polling.
- Stop observation through a bounded `epoll_wait` interval followed by active-connection drain/retirement.
- Runtime counters for accepted, rejected, completed, failed, active, and peak-active connections.
- Cross-platform feature detection through `EpollRuntime::supported()`; non-Linux `run()` fails explicitly instead of silently degrading.

### Stress/performance evidence harness

- `vhttp_bench_server` currently exercises the real thread-pool/parser/router/serializer path with configurable workers, pending capacity, fixed run duration, and response payload size.
- `tools/stress_http.py` uses only the Python standard library.
- Persistent-connection and connection-churn load modes.
- Configurable request count, concurrency, warm-up, timeout, expected status, and tolerated error rate.
- Throughput plus min/mean/p50/p95/p99/max request latency.
- Success/failure, HTTP status, and top transport-error accounting.
- Machine-readable JSON output with client command and environment metadata.
- Explicit methodology in [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md).
- No fabricated throughput/latency numbers are committed.

M6B.1 will put the thread-pool and `epoll` runtimes behind the **same** benchmark server surface before any comparative performance claim is allowed.

## Verification

- Unit/adversarial tests for parser, framing, responses, routing, connection policy, connection-session state, static files, and runtime configuration.
- Filesystem tests for traversal, percent-encoding, MIME, validators, ranges, and symlink escapes where the platform permits symlink creation.
- Real loopback TCP tests for persistent/pipelined requests, chunked flows, static `GET`/`HEAD`/`206`/`304`/`416`, concurrent handler overlap, queue saturation, and graceful active-request drain.
- Linux `epoll` loopback tests for ordered pipelining, admission pressure, idle retirement, bounded response buffering, and stop/drain behavior.
- CI for GCC, Clang, and MSVC.
- Linux GCC/Clang compile and execute the real event-loop tests.
- Windows MSVC compiles the public `EpollRuntime` API/non-Linux fallback and preserves the full existing cross-platform test suite.
- CI compiles the benchmark server and syntax-checks the dependency-free load harness; performance numbers are not treated as stable CI assertions.

## Architecture

```text
                         handler / router / static files
                                   ^
                                   |
                           ConnectionSession
                       parser + lifecycle + serializer
                          ^                    ^
                          |                    |
              blocking Server           Linux EpollRuntime
                    ^                    nonblocking I/O
                    |
          +---------+----------+
          |                    |
     serial accept      bounded ThreadPoolRuntime
```

The runtime changes **transport scheduling**, not HTTP semantics. See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md), [`docs/SECURITY.md`](docs/SECURITY.md), [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md), and [`docs/ROADMAP.md`](docs/ROADMAP.md).

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

On Windows with Visual Studio:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Run the existing example

```bash
./build/vhttp_hello 8080 ./public
```

The first argument is the port. The optional second argument is a document root exposed under `/static`.

Windows:

```powershell
.\build\Release\vhttp_hello.exe 8080 .\public
```

Example requests:

```bash
curl -i http://127.0.0.1:8080/health
curl -i http://127.0.0.1:8080/chunked
curl -i -X POST --data-binary 'hello' http://127.0.0.1:8080/echo
curl -i http://127.0.0.1:8080/static/index.html
curl -I http://127.0.0.1:8080/static/index.html
curl -i -H 'Range: bytes=0-99' http://127.0.0.1:8080/static/index.html
```

## Thread-pool embedding

```cpp
#include "vhttp/server/thread_pool_runtime.hpp"

vhttp::server::ThreadPoolConfig pool;
pool.worker_count = 8;
pool.max_pending_connections = 256;

vhttp::server::ThreadPoolRuntime runtime(handler, {}, pool);
runtime.run("0.0.0.0", 8080);  // blocks until request_stop()
```

A controlling thread can call `request_stop()`. The accept loop observes it through bounded polling, then queued/active connections drain and every worker is joined.

**Handler concurrency contract:** thread-pool handlers may execute concurrently. Shared mutable application state must be synchronized by the application.

## Linux `epoll` embedding

```cpp
#include "vhttp/server/epoll_runtime.hpp"

vhttp::server::EpollConfig event_config;
event_config.max_connections = 4096;
event_config.max_pending_output_bytes = 16 * 1024 * 1024;

vhttp::server::EpollRuntime runtime(handler, {}, event_config);
runtime.run("0.0.0.0", 8080);  // Linux only; blocks until stop + drain
```

Use `EpollRuntime::supported()` before selecting this runtime in cross-platform applications.

**Event-loop handler contract:** handlers execute synchronously on the event-loop thread. A slow handler, blocking filesystem operation, or expensive computation can stall unrelated event-loop connections. The current static-file handler is synchronous as well.

## Generate local performance evidence

The current benchmark server targets the thread-pool baseline:

```bash
./build/vhttp_bench_server 8081 4 256 60 128
```

Persistent-connection baseline:

```bash
python3 tools/stress_http.py --port 8081 --requests 10000 --concurrency 8 --warmup 500 --mode keepalive --json-out benchmark-results/local/keepalive-c8.json
```

Connection-churn/admission pressure:

```bash
python3 tools/stress_http.py --port 8081 --requests 10000 --concurrency 64 --warmup 500 --mode connect --json-out benchmark-results/local/connect-c64.json
```

See [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md) before publishing or comparing results. Local loopback numbers are not universal capacity claims. M6B.1 will add a common thread-pool/`epoll` selector so the exact same protocol can compare both runtimes.

## Engineering constraints and limitations

The server does **not** use Boost.Beast, Crow, cpp-httplib, Drogon, Pistache, or another HTTP server framework.

The repository will not claim production readiness or high-performance superiority until broader fuzzing, soak/stress work, filesystem-race hardening, and reproducible comparative resource evidence exist.

Important current limitations:

- the worker-pool runtime is blocking: one slow/persistent connection occupies one worker.
- the `epoll` runtime is event-driven at the transport layer, but handlers still execute synchronously on the event-loop thread.
- queue/admission saturation closes excess accepted transports instead of returning an HTTP `503`.
- event-runtime responses larger than `max_pending_output_bytes` are failed/retired rather than streamed.
- graceful drain waits for active handlers/connections rather than forcibly cancelling them.
- request bodies and static GET payloads are assembled/read in memory under configured limits.
- static serving supports one range, weak metadata ETags, and canonicalize-then-open confinement that is not race-free against hostile concurrent local filesystem mutation.
- the current event-loop idle-deadline implementation scans active connections after bounded `epoll_wait` intervals rather than using a timer wheel/deadline heap.
- the first-party Python harness can become the client-side bottleneck at high request rates and is intended primarily for transparent regression/saturation evidence.
- no zero-copy file path or Windows IOCP backend yet.

## Next milestone

**M6B.1 — comparative event-runtime evidence:** expose thread-pool and `epoll` modes through one benchmark-server interface, run identical workload matrices, and record repeatable throughput/latency plus CPU/RSS/peak-active evidence tied to exact machine/build metadata. Only after that gate may the repository claim a measured performance difference.

Windows IOCP follows as M6C.

## Milestone evidence

- [`docs/MILESTONE_1.md`](docs/MILESTONE_1.md)
- [`docs/MILESTONE_2.md`](docs/MILESTONE_2.md)
- [`docs/MILESTONE_3.md`](docs/MILESTONE_3.md)
- [`docs/MILESTONE_4.md`](docs/MILESTONE_4.md)
- [`docs/MILESTONE_5.md`](docs/MILESTONE_5.md)
- [`docs/MILESTONE_6A.md`](docs/MILESTONE_6A.md)
- [`docs/MILESTONE_6A1.md`](docs/MILESTONE_6A1.md)
- [`docs/MILESTONE_6B.md`](docs/MILESTONE_6B.md)

## License

MIT.
