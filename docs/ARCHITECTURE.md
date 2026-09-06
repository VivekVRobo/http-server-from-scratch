# Architecture

## Shared HTTP connection state machine

All runtimes now share the same transport-agnostic `ConnectionSession` for HTTP/1.x lifecycle semantics:

```text
received bytes
    |
    v
ConnectionSession
    |
    +--> incremental parser / framing
    |
    +--> persistence + request ceiling
    |
    +--> application handler
    |
    +--> authoritative response serializer
    |
    v
one serialized response at a time
```

`ConnectionSession` performs no socket I/O. A runtime feeds received bytes, writes the returned serialized response, then calls `on_write_complete()`. If pipelined request bytes were already received, the next request is dispatched only after the previous response has been fully acknowledged as written. This preserves response order and prevents a runtime from accumulating an unbounded chain of serialized pipelined responses.

The blocking `Server::serve_connection()` is implemented on top of this session state machine, so the Linux event runtime does not fork HTTP parser, persistence, `HEAD`, request-limit, or serializer behavior.

## Blocking and thread-pool runtime

```text
                TcpListener
                    |
              accept_for(timeout)
                    |
       +------------+-------------+
       |                          |
 queue has capacity           queue full
       |                          |
       v                          v
 bounded TcpStream queue      close + reject++
       |
       +-------+-------+--- ...
               |
        fixed worker threads
         |      |      |
         v      v      v
      serve_connection()
         |      |      |
         +------v------+
                |
        ConnectionSession
```

`accept_for()` uses cross-platform readiness with a bounded poll interval. `request_stop()` stops future acceptance, queued and active work drain under normal connection timeout behavior, and all worker threads are joined.

### Thread-pool lifecycle invariants

1. Worker count and pending-queue capacity are fixed before `run()` starts.
2. The queue never exceeds its configured bound.
3. An accepted stream that cannot be queued is closed immediately and counted as rejected.
4. Workers are joinable threads; there are no detached connection lifetimes.
5. `request_stop()` stops future acceptance after at most one accept-poll interval.
6. Stop transitions workers into drain mode: queued streams are processed, active streams are allowed to finish, then workers exit and are joined.
7. One connection failure increments the failed counter but cannot terminate another worker.
8. Runtime statistics are concurrency-safe snapshots.
9. Application handlers may execute concurrently; synchronization of shared mutable handler state is the application's responsibility.

## Linux `epoll` runtime

```text
nonblocking listener
       |
       v
     epoll
       |
  +----+--------------------+
  |                         |
EPOLLIN                  EPOLLOUT
  |                         |
accept/read             partial write
  |                         |
  v                         |
ConnectionSession <----------+
  |
  +--> response ready --> bounded output buffer
  |
  `--> need more data --> read interest
```

The Linux event runtime is level-triggered and single-event-loop-threaded. Accepted sockets are nonblocking and CLOEXEC. A connection is interested in reads only while its `ConnectionSession` needs more input. Once a response is ready, read interest is removed and only write readiness is observed until that response is fully flushed. This is the runtime's primary per-connection output backpressure mechanism.

### `epoll` lifecycle invariants

1. Active accepted connections never exceed `EpollConfig::max_connections`.
2. Excess accepted transports are closed immediately and counted as rejected.
3. At most one serialized response is buffered per connection.
4. A serialized response larger than `max_pending_output_bytes` fails and retires that connection rather than allowing unbounded event-loop output memory.
5. Partial writes retain an offset and resume only on later write readiness.
6. Pipelined requests are dispatched in order through `ConnectionSession`; a later response is never emitted before the previous response write completes.
7. Idle read or write-side connections are retired under the same `ConnectionConfig::idle_timeout` policy.
8. `request_stop()` is observed within the configured `epoll_wait` polling bound, closes the listener, and then drains/retires existing connections.
9. Runtime counters expose accepted, rejected, completed, failed, active, and peak-active connections.
10. One connection-level parsing, handler, or transport failure is contained to that connection.

## Shared HTTP invariants

- HTTP/1.1 persistence / HTTP/1.0 keep-alive policy is shared across blocking and event runtimes.
- Request framing remains incremental, unambiguous, and bounded.
- Pipelined responses remain ordered within a connection.
- `HEAD` suppresses payload bytes while preserving GET-equivalent metadata.
- Handler-requested `Connection: close` and the max-request ceiling remain authoritative.
- Parser errors produce one closing `400 Bad Request` response when the transport can still write it.
- Routing and static-file semantics are invoked through the same application handler path.

## Important event-loop boundary

`EpollRuntime` is event-driven at the **transport** layer, but application handlers are still called synchronously on the event-loop thread. A slow handler, blocking filesystem operation, or expensive computation can therefore stall unrelated event-loop connections. The secure static-file handler is also currently synchronous. This is an explicit limitation and one reason the repository does not yet claim production-grade event-loop scalability.

The current idle-deadline implementation scans active connections after bounded `epoll_wait` intervals. It avoids busy polling but is intentionally simpler than a timer wheel or deadline heap. A more advanced deadline structure can be justified later by profiling rather than added speculatively.

## Verification boundary

The thread-pool suite uses real loopback TCP clients to verify concurrent handler overlap, bounded queue saturation, and graceful drain.

The Linux event-runtime suite uses real loopback TCP clients to verify:

- persistent/pipelined response ordering,
- bounded connection admission and rejection accounting,
- idle timeout retirement,
- bounded pending-response memory,
- graceful stop while an active response is being produced,
- configuration validation.

All earlier parser, framing, router, connection, session, and static-file tests remain in the same CTest suite. Windows CI also compiles the public `EpollRuntime` API and non-Linux fallback so the Linux-specific feature cannot silently break cross-platform builds.

## Planned evolution

1. ✅ Blocking socket + HTTP parser/serializer.
2. ✅ Routing/request semantics.
3. ✅ Persistent HTTP/1.x lifecycle.
4. ✅ Chunked framing hardening.
5. ✅ Secure static-file semantics.
6. ✅ Bounded fixed blocking worker pool.
7. ✅ Shared transport-agnostic connection session.
8. ✅ Linux nonblocking `epoll` runtime correctness.
9. Comparative thread-pool vs `epoll` benchmark/resource evidence.
10. Windows IOCP runtime with completion/cancellation lifetime rules.
11. Fuzzing, sanitizers, soak testing, and deeper performance evidence.
