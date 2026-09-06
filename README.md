# vhttp — HTTP/1.1 Server From Scratch

A cross-platform C++20 HTTP server built from raw sockets to make protocol parsing, routing, connection lifecycle, message framing, static-file security, bounded concurrency, event-driven I/O, and performance verification visible and testable.

> Status: **Milestone 6B.1 comparative runtime evidence workflow implemented.** The repository contains a bounded blocking thread-pool runtime and a Linux nonblocking `epoll` runtime sharing the same HTTP connection state machine, plus one apples-to-apples benchmark surface and repeated-evidence orchestrator. No runtime-performance winner is claimed until controlled-host evidence is published.

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
- Runtime counters for accepted, rejected, completed, failed, active, queued, and peak-active connections.
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

### Comparative performance evidence workflow

- `vhttp_bench_server` selects `threadpool` or Linux `epoll` through one explicit CLI.
- Both modes use the same `/bench` route and payload.
- A common total accepted in-flight admission budget is used for comparison.
- `--until-stdin` lets an orchestrator stop the server exactly when one client phase finishes.
- Server JSON can record Git revision, build configuration, compiler, runtime configuration, wall/process-CPU time, Linux peak RSS, and runtime counters.
- `tools/stress_http.py` provides dependency-free keep-alive and connection-churn load with throughput, failures, status/error accounting, and attempt/success latency distributions.
- `tools/compare_runtimes.py` repeats scenarios, alternates runtime execution order, preserves every raw client/server JSON and log, and writes median summaries.
- Primary comparison latency uses successful-request p50/p95/p99 while preserving attempt latency and failure-rate evidence.
- CI runs a tiny end-to-end comparison smoke but asserts no throughput/latency threshold and is explicitly **not benchmark evidence**.
- No fabricated throughput/latency values and no automatic runtime winner are committed.

See [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md) and [`benchmark-results/README.md`](benchmark-results/README.md) for the evidence rules.

## Verification

- Unit/adversarial tests for parser, framing, responses, routing, connection policy, connection-session state, static files, and runtime configuration.
- Filesystem tests for traversal, percent-encoding, MIME, validators, ranges, and symlink escapes where the platform permits symlink creation.
- Real loopback TCP tests for persistent/pipelined requests, chunked flows, static `GET`/`HEAD`/`206`/`304`/`416`, concurrent handler overlap, queue saturation, and graceful active-request drain.
- Linux `epoll` loopback tests for ordered pipelining, admission pressure, idle retirement, bounded response buffering, and stop/drain behavior.
- Thread-pool tests verify concurrency, saturation, drain, and peak-active accounting.
- CI for GCC, Clang, and MSVC.
- Linux GCC/Clang compile and execute the real event-loop tests.
- Windows MSVC compiles the public `EpollRuntime` API/non-Linux fallback and preserves the cross-platform suite.
- Linux GCC CI runs one tiny threadpool+epoll comparison smoke to verify benchmark orchestration end to end.

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

## Compare thread pool vs Linux `epoll`

Build Release first, then run the comparison orchestrator on Linux:

```bash
python3 tools/compare_runtimes.py \
  --server ./build/vhttp_bench_server \
  --output-dir benchmark-results/local/keepalive-c8 \
  --runtimes threadpool,epoll \
  --runs 5 \
  --admission 260 \
  --workers 4 \
  --payload 128 \
  --requests 10000 \
  --concurrency 8 \
  --warmup 500 \
  --mode keepalive \
  --require-identified-build
```

Connection-churn variant:

```bash
python3 tools/compare_runtimes.py \
  --server ./build/vhttp_bench_server \
  --output-dir benchmark-results/local/connect-c64 \
  --runtimes threadpool,epoll \
  --runs 5 \
  --admission 260 \
  --workers 4 \
  --payload 128 \
  --requests 10000 \
  --concurrency 64 \
  --warmup 500 \
  --mode connect \
  --require-identified-build
```

Every run retains raw client/server JSON and logs. `summary.json` contains medians and execution order but intentionally does not declare a winner.

Local scratch results remain under ignored `benchmark-results/local/`. Only reviewed, complete bundles tied to a documented machine and Git revision belong under `benchmark-results/curated/`.

## Engineering constraints and limitations

The server does **not** use Boost.Beast, Crow, cpp-httplib, Drogon, Pistache, or another HTTP server framework.

The repository will not claim production readiness or general performance superiority until broader fuzzing, soak/stress work, filesystem-race hardening, and reproducible controlled-host resource evidence exist.

Important current limitations:

- the worker-pool runtime is blocking: one slow/persistent connection occupies one worker.
- the `epoll` runtime is event-driven at the transport layer, but handlers still execute synchronously on the event-loop thread.
- queue/admission saturation closes excess accepted transports instead of returning an HTTP `503`.
- event-runtime responses larger than `max_pending_output_bytes` are failed/retired rather than streamed.
- graceful drain waits for active handlers/connections rather than forcibly cancelling them.
- request bodies and static GET payloads are assembled/read in memory under configured limits.
- static serving supports one range, weak metadata ETags, and canonicalize-then-open confinement that is not race-free against hostile concurrent local filesystem mutation.
- the current event-loop idle-deadline implementation scans active connections after bounded `epoll_wait` intervals rather than using a timer wheel/deadline heap.
- the first-party Python harness can become the client-side bottleneck at high request rates.
- the `/bench` handler is intentionally tiny; its results do not prove behavior for blocking/expensive handlers.
- no zero-copy file path or Windows IOCP backend yet.

## Next milestone

**M6B.2 — controlled-host comparative evidence:** run the documented repeated keep-alive and connection-churn matrices on a named Linux machine, preserve all raw evidence, and commit only reviewed bundles under `benchmark-results/curated/`. Only then may the project make a measured runtime-performance claim for those specific conditions.

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
- [`docs/MILESTONE_6B1.md`](docs/MILESTONE_6B1.md)

## License

MIT.
