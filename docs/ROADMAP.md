# Roadmap

## M0 — repository foundation ✅
- C++20 + CMake
- Linux GCC/Clang CI
- Windows MSVC CI
- architecture/security documentation

## M1 — blocking HTTP baseline ✅
- cross-platform TCP listener and stream RAII
- incremental request parser
- request-line + headers + `Content-Length` body
- response serializer
- parser fragmentation tests

## M2 — routing and request semantics ✅
- method-aware route trie
- static and parameterized paths
- query/path parameters
- `404` / `405`, deterministic `Allow`, `HEAD`, automatic `OPTIONS`

## M3 — HTTP/1.1 connection lifecycle ✅
- HTTP/1.1 persistence / HTTP/1.0 keep-alive opt-in
- multiple requests per socket
- pipelined-byte preservation
- request ceiling + receive idle timeout
- deterministic closure and loopback tests

## M4 — message framing ✅
- incremental chunked request decoder
- bounded extensions/trailers
- framing ambiguity hardening
- chunked response encoder
- adversarial and real TCP framing tests

## M5 — secure static file engine ✅
- document-root confinement and traversal/symlink escape checks
- MIME + nosniff
- ETag/date conditionals
- single ranges with `206` / `416`
- `GET` / `HEAD` parity and filesystem/TCP tests

## M6A — bounded blocking concurrency ✅
- fixed worker pool
- bounded accepted-connection queue
- queue-saturation rejection accounting
- timed accept polling for deterministic stop observation
- graceful queued/active drain and worker join
- runtime statistics and concurrency/backpressure tests

## M6A.1 — stress/performance evidence harness ✅
- benchmark server backed by the real thread-pool/parser/router/serializer path
- persistent-connection and connection-churn load modes
- warm-up, configurable request/concurrency/timeout/error policy
- throughput and min/mean/p50/p95/p99/max latency reporting
- status/error accounting and machine-readable JSON evidence
- documented benchmark matrix, repetition rules, and interpretation limits
- no committed synthetic benchmark claims

## M6B — Linux event-runtime correctness ✅
- transport-agnostic `ConnectionSession` shared with blocking runtime
- nonblocking Linux listener and accepted sockets
- level-triggered `epoll` accept/read/write readiness
- partial-write handling with read-side backpressure while output is pending
- bounded active-connection admission and explicit rejection accounting
- bounded per-connection serialized-output budget
- idle timeout retirement without busy polling
- deterministic stop: stop accepting, then drain/retire active connections
- ordered persistent/pipelined HTTP behavior through the shared session core
- Linux loopback tests for parity, admission pressure, idle retirement, output bounds, and drain
- GCC/Clang Linux verification plus non-Linux API compilation on MSVC

## M6B.1 — comparative event-runtime evidence ← next
- allow the benchmark server to select thread-pool vs `epoll` runtime on Linux
- run identical keep-alive and connection-churn matrices through the existing harness
- collect repeated throughput and p50/p95/p99 latency results
- collect CPU/RSS and peak-active-connection evidence with exact machine/build metadata
- commit only curated, reproducible result sets tied to Git revisions
- make no scalability/performance superiority claim before this evidence gate passes

## M6C — Windows event runtime
- IOCP accept/read/write completion model
- per-connection operation lifetime rules
- cancellation/drain semantics
- correctness parity with Linux/event and blocking runtimes
- compare against the same benchmark protocol

## M7 — verification and deeper performance evidence
- fuzzing + sanitizers
- malformed-request corpus
- external high-rate load harness such as `wrk`
- deeper CPU/RSS/resource profiling
- long-duration soak tests and failure-injection scenarios
