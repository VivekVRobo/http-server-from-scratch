# Security model

This repository is currently an educational implementation, not a production reverse proxy.

## Protections already present

### Request syntax and message framing

- Bounded request line, headers, header count, body, chunk line, trailer bytes, and trailer count.
- HTTP/1.1 requests require exactly one non-empty `Host` field; missing, empty, and duplicate field lines are rejected.
- Conflicting `Content-Length` rejection.
- `Transfer-Encoding` + `Content-Length` ambiguity rejection.
- Only supported HTTP/1.1 chunked request coding is accepted; unsupported coding chains fail closed.
- Chunk-size overflow/body-budget checks and exact data CRLF validation.
- Framing/routing-sensitive trailer fields are rejected.
- Serializer-authoritative response framing; body-forbidden statuses do not emit payload framing/body bytes.

### Connection lifecycle

- Configurable max requests per persistent connection.
- Cross-platform receive idle timeout.
- Idle/partial peers are retired instead of held forever.
- Case-insensitive comma-token `Connection` policy.
- Parse failures receive `400` and deterministic close.

### Static file serving

- Explicit URL prefix + canonical document root.
- Strict percent-decoding; separator, backslash, NUL/control, and dot-segment traversal rejection.
- Canonical candidate containment check after symlink/reparse resolution.
- Regular-file-only serving and explicit index policy.
- Bounded file size, MIME `nosniff`, conditionals, and bounded single-range parsing.
- Known limitation: canonicalize-then-open confinement is not race-free against hostile concurrent local filesystem mutation.

### Bounded concurrency

- Fixed worker count; no detached connection threads.
- Accepted-connection queue has a fixed maximum size rather than growing without bound.
- Saturated transports are closed and counted rather than retained indefinitely.
- Stop requests end new acceptance and then drain queued/active connections before worker join.
- Per-connection exceptions are contained inside worker threads.
- Runtime counters expose accepted/rejected/completed/failed/active/queued connections for observability.
- Timed listener polling avoids unsafe cross-thread listener mutation solely to wake a blocking `accept()`.

## Concurrency trust boundary

The HTTP transport/parser state remains connection-local, but the application `Handler` may now execute concurrently when `ThreadPoolRuntime` is used. Handlers that capture mutable shared state must provide their own synchronization. The runtime cannot make arbitrary application callbacks data-race-free.

## Known limitations / planned hardening

- A slow/persistent connection occupies one blocking worker in the thread-pool runtime; event-driven backends are still required for large connection counts.
- Queue saturation currently closes excess accepted TCP transports rather than parsing enough of the request to emit `503`.
- Graceful drain does not forcibly cancel active connections; receive-idle policy bounds silent peers, while long-running handlers remain application-controlled.
- Static GET uses bounded in-memory reads; no race-resistant descriptor/handle-relative open or zero-copy transfer yet.
- Weak metadata ETags and one range per response remain deliberate limitations.
- Distinct total header/body deadlines, global admission policy, fuzzing, sanitizers, and malformed-request corpus work remain planned.

Do not deploy the current milestone directly on an untrusted public network.
