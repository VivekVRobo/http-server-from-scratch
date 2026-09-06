# Milestone 6B — Linux `epoll` Event Runtime

## Status

**Correctness implementation complete; comparative performance evidence pending.**

The Linux event runtime has passed real loopback verification on both GCC and Clang CI. The public API and non-Linux fallback also compile cleanly under MSVC, with the full existing Windows test suite preserved.

This milestone does **not** claim that `epoll` is faster, more scalable, or production-ready. Those claims remain blocked on the M6B.1 comparative evidence gate.

## Delivered

- Transport-agnostic `ConnectionSession` shared by blocking and event runtimes.
- Nonblocking Linux listener and accepted sockets.
- CLOEXEC descriptors.
- Level-triggered `epoll` accept/read/write readiness.
- Bounded active-connection admission via `max_connections`.
- Explicit rejection accounting when admission is saturated.
- Partial-write tracking with offset-based resume.
- `MSG_NOSIGNAL` writes to contain peer-close behavior without process-wide SIGPIPE handling.
- Read-side backpressure while a serialized response is pending.
- One serialized response buffered per connection.
- Configurable `max_pending_output_bytes` guard.
- Existing HTTP persistence, pipelining, `HEAD`, parser-error, and request-ceiling behavior reused through `ConnectionSession`.
- Idle connection retirement under `ConnectionConfig::idle_timeout`.
- Bounded `epoll_wait` polling so `request_stop()` can be observed deterministically without cross-thread descriptor mutation.
- Stop/drain lifecycle: listener closes first, active connections then complete or retire under normal timeout/peer behavior.
- Runtime statistics for accepted, rejected, completed, failed, active, and peak-active connections.
- Cross-platform `EpollRuntime::supported()` feature detection and an explicit non-Linux runtime error.

## Verification

The Linux event-runtime test suite starts a real listener on an ephemeral loopback port and verifies:

1. **Persistent + pipelined ordering** — two requests sent on one connection produce ordered responses through the shared session core.
2. **Admission backpressure** — a configured one-connection limit keeps one partial request active and rejects the next accepted transport deterministically.
3. **Idle retirement** — a silent connection is retired by the configured idle timeout without a busy loop.
4. **Output memory bound** — a response larger than the configured pending-output budget fails and retires only that connection.
5. **Graceful stop/drain** — a stop request arriving while a response is being produced does not discard that response.
6. **Configuration validation** — invalid connection capacity, output budget, and polling interval are rejected before the runtime starts.

All earlier parser, framing, routing, blocking connection, connection-session, static-file, and thread-pool tests remain enabled alongside the new event-loop tests.

## Event-loop invariants

- A connection is read-interested only while the shared session needs more input.
- While output is pending, the runtime switches to write interest and does not continue consuming arbitrary new socket input.
- A later pipelined request cannot dispatch until the previous response write is acknowledged complete.
- The active connection map never intentionally exceeds the configured admission bound.
- A per-connection serialized response cannot exceed the configured event-runtime output budget.
- Connection-level failures are contained; they do not terminate the event loop.

## Important limitations

- Application handlers execute synchronously on the event-loop thread. Slow handlers can stall unrelated connections.
- Static-file reads are synchronous and can block the event loop.
- The current deadline implementation scans active connections after bounded `epoll_wait` intervals rather than using a timer wheel/deadline heap.
- Admission saturation closes the excess accepted TCP transport rather than parsing enough HTTP to synthesize a `503`.
- Oversized event-runtime responses are failed/retired rather than streamed.
- There is no zero-copy/static-file send path yet.
- Windows IOCP remains a separate milestone.
- No throughput, latency, CPU, RSS, or scalability superiority claim is made by this milestone.

## M6B.1 evidence gate

Before the repository claims a performance or scalability advantage, it must:

1. expose thread-pool and `epoll` modes through the same benchmark server surface,
2. run identical keep-alive and connection-churn workload matrices,
3. repeat each scenario enough times to report representative variance,
4. record throughput and p50/p95/p99 latency,
5. record CPU, RSS, peak-active-connection, build, OS, CPU, and exact command metadata,
6. tie curated evidence to a Git revision,
7. document regressions and failure cases rather than publishing only favorable runs.

Until that gate passes, M6B is a **correctness milestone**, not a benchmark claim.
