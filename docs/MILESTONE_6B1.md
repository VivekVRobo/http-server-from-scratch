# Milestone 6B.1 — Comparative Runtime Evidence Workflow

## Status

**Implementation complete; controlled benchmark publication remains a separate evidence gate.**

Milestone 6B established Linux `epoll` correctness. M6B.1 makes the blocking thread pool and Linux event runtime directly comparable under one benchmark surface without changing the application handler, payload, client harness, or measurement schema between runtimes.

This milestone does **not** claim that one runtime is faster or more scalable. It delivers the mechanism required to make that claim defensibly later.

## Delivered

### One benchmark server for both runtimes

`vhttp_bench_server` now accepts:

```text
--runtime threadpool
--runtime epoll
```

Both modes expose the same `/bench` route and response payload.

The benchmark server also provides:

- explicit port selection,
- configurable payload size,
- a common total admission budget,
- thread-pool worker-count selection,
- fixed-duration operation,
- `--until-stdin` exact orchestration mode,
- optional machine-readable server evidence JSON.

### Common admission model

The comparison uses one total accepted in-flight connection budget:

```text
epoll max_connections = admission
threadpool workers + pending queue = admission
```

The thread-pool pending capacity is therefore:

```text
admission - workers
```

This equalizes the configured accepted-connection budget while preserving the real scheduling differences between the two runtimes.

### Normalized runtime evidence

The thread-pool runtime now records `peak_active`, matching the Linux event runtime's equivalent metric.

Both benchmark modes can therefore report normalized:

- accepted,
- rejected,
- completed,
- failed,
- active,
- peak-active connections.

Thread-pool evidence additionally records the final queued count.

### Build and resource identity

The benchmark target records:

- Git revision when Git is available at configure time,
- build configuration,
- compiler/version,
- platform,
- process wall time,
- process CPU seconds,
- Linux peak RSS where available.

Strict warning flags are also applied directly to the benchmark target.

### Exact client/server measurement window

`--until-stdin` allows the orchestrator to:

1. launch the benchmark server,
2. wait for its explicit `READY` marker,
3. run exactly one client phase,
4. signal graceful stop immediately after the client completes,
5. wait for runtime drain,
6. collect final server resource/runtime evidence.

This removes the previous mismatch where a fixed server sleep could include arbitrary idle time after the client finished.

### Comparison orchestrator

`tools/compare_runtimes.py`:

- runs the same client scenario against each selected runtime,
- uses a fresh loopback port per repetition,
- reuses `tools/stress_http.py`,
- preserves every client JSON/log,
- preserves every server JSON/log,
- repeats scenarios (default five times),
- alternates runtime order across repetitions,
- validates the expected evidence schema before summarizing,
- can require an identified Git build,
- writes `summary.json` with medians and raw-run references.

### Summary metrics

The orchestrator records medians for:

- requests/second,
- successful requests/second,
- failure rate,
- successful-request p50/p95/p99 latency,
- attempt p50/p95/p99 latency,
- process CPU seconds,
- wall seconds,
- peak RSS,
- peak-active connections,
- rejected connections,
- failed connections.

Successful-request latency is the primary latency basis, while attempt latency remains available so failures cannot be hidden.

### Ordering discipline

With multiple runtimes selected, execution order alternates by repetition:

```text
repetition 1: threadpool -> epoll
repetition 2: epoll -> threadpool
repetition 3: threadpool -> epoll
...
```

This reduces systematic first-run ordering bias. It is not presented as a substitute for a controlled host.

## CI verification

Linux CI:

- Python-compiles both benchmark harnesses,
- checks both `--help` surfaces,
- builds with GCC and Clang,
- runs the full CTest suite,
- runs one tiny threadpool+epoll comparison smoke on GCC.

Windows CI continues to build the shared benchmark binary and the non-Linux `EpollRuntime` fallback under MSVC.

The CI comparison is explicitly labeled **not benchmark evidence** and contains no throughput/latency threshold or runtime-winner assertion.

## Evidence integrity rules

The comparison workflow deliberately does **not**:

- choose a winning runtime,
- compute a promotional "X% faster" headline,
- discard failed repetitions,
- treat hosted CI measurements as publishable performance evidence,
- infer internet-facing capacity from loopback,
- fill unavailable resource fields with guessed values.

## Why controlled measurements remain separate

A defensible runtime-performance comparison depends on details that cannot be manufactured by repository code:

- CPU model and core topology,
- RAM,
- Linux/kernel version,
- compiler/toolchain,
- Release build identity,
- power/performance mode,
- background workload,
- same-host versus remote client placement,
- repeated raw results.

For that reason M6B.1 closes the **measurement workflow** while M6B.2 remains the controlled-host **measurement publication** gate.

## Next evidence gate — M6B.2

Before making a measured performance/scalability claim:

1. select a named Linux machine,
2. build Release from an exact Git revision,
3. record environment metadata,
4. run both keep-alive and connection-churn matrices,
5. repeat each scenario at least five times,
6. preserve every raw JSON/log,
7. review failures and saturation behavior,
8. copy complete reviewed bundles into `benchmark-results/curated/`,
9. publish only conclusions supported by those bundles.

See [`PERFORMANCE.md`](PERFORMANCE.md) and [`../benchmark-results/README.md`](../benchmark-results/README.md).
