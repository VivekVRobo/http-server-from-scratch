# Performance and Stress Evidence

This document defines how performance evidence for `vhttp` should be collected, compared, and reported.

The repository does **not** claim production readiness or universal throughput numbers. Results from this harness are local measurements that are meaningful only when the server revision, build, machine, operating system, compiler, runtime configuration, load shape, and client methodology are recorded.

## First-party evidence tools

Three first-party tools now provide the complete comparison path:

- `vhttp_bench_server`: one benchmark server that can run the bounded `threadpool` runtime or the Linux `epoll` runtime behind the same `/bench` handler and payload.
- `tools/stress_http.py`: a Python-standard-library load generator that records throughput, successes/failures, HTTP status counts, transport errors, and both attempt/success latency distributions.
- `tools/compare_runtimes.py`: an orchestrator that launches both runtimes against the same scenario, preserves raw client/server evidence, alternates execution order across repetitions, and writes median summaries without declaring a winner.

The harness is designed to answer questions such as:

- How do the blocking thread pool and Linux `epoll` runtime behave under the **same** keep-alive workload?
- How do they behave under connection churn and admission pressure?
- At what offered concurrency does latency rise sharply?
- Does either runtime reject/fail connections under a configured admission bound?
- What server process CPU time, Linux peak RSS, and peak-active-connection behavior accompanies the client-observed throughput/latency?
- Does a code change regress one runtime under an otherwise identical local scenario?

It is **not** a substitute for later external-host load generation, high-rate tools such as `wrk`, CPU profiling, sanitizers/fuzzing, or internet-facing capacity testing.

## Build

Use a Release build for measurements.

Linux:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Windows:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
```

When Git is available at configure time, the benchmark binary embeds the repository revision. The build configuration and compiler are also written into server evidence JSON.

## Shared benchmark-server interface

### Thread pool

```bash
./build/vhttp_bench_server \
  --runtime threadpool \
  --port 8081 \
  --workers 4 \
  --admission 260 \
  --payload 128 \
  --duration 60 \
  --stats-json benchmark-results/local/threadpool-server.json
```

### Linux `epoll`

```bash
./build/vhttp_bench_server \
  --runtime epoll \
  --port 8081 \
  --admission 260 \
  --payload 128 \
  --duration 60 \
  --stats-json benchmark-results/local/epoll-server.json
```

The `epoll` mode is Linux-only.

### Common admission budget

`--admission` is the total accepted in-flight connection budget used for comparison.

For `epoll`:

```text
max_connections = admission
```

For the thread pool:

```text
active worker capacity = workers
pending queue capacity = admission - workers
```

This normalizes the **total accepted in-flight connection budget**. It does not make the two schedulers identical, and it must not be described as doing so. Their execution models remain fundamentally different.

The thread-pool configuration therefore requires:

```text
admission > workers
```

## Exact orchestration mode

Manual fixed-duration runs are useful, but automated comparisons need server resource statistics to cover the same client phase rather than an arbitrary extra sleep.

`vhttp_bench_server --until-stdin` starts normally, prints a machine-readable readiness marker, and then stops gracefully after receiving one input line or EOF. `tools/compare_runtimes.py` uses this mode so the server is stopped immediately after the client phase finishes and final runtime/resource evidence is then written.

## Run one client manually

Persistent connection:

```bash
python3 tools/stress_http.py \
  --host 127.0.0.1 \
  --port 8081 \
  --path /bench \
  --requests 10000 \
  --concurrency 8 \
  --warmup 500 \
  --mode keepalive \
  --json-out benchmark-results/local/client.json
```

Connection churn:

```bash
python3 tools/stress_http.py \
  --host 127.0.0.1 \
  --port 8081 \
  --path /bench \
  --requests 10000 \
  --concurrency 64 \
  --warmup 500 \
  --mode connect \
  --json-out benchmark-results/local/client-connect.json
```

`keepalive` gives each client worker a persistent `HTTPConnection`. `connect` creates a fresh TCP connection for every measured request and therefore includes connection setup in measured latency while increasing accept/admission pressure.

By default the client exits non-zero if any measured request fails or returns an unexpected status. For an intentional saturation experiment, increase `--max-error-rate` explicitly and keep that policy with the published evidence.

## Automated repeated comparison

A normal comparison should use the orchestrator rather than manually timing server/client processes.

### Keep-alive example

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

### Connection-churn example

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

The orchestrator preserves, for every run:

```text
<runtime>-run-XX-client.json
<runtime>-run-XX-server.json
<runtime>-run-XX-client.log
<runtime>-run-XX-server.log
```

It also writes:

```text
summary.json
```

The summary records the scenario, orchestrator environment, raw-run filenames, execution order, Git/build/compiler identities reported by the server, and median metrics.

## Execution-order policy

When multiple runtimes are selected, the orchestrator alternates their order across repetitions.

Example for two runtimes and four repetitions:

```text
run 1: threadpool -> epoll
run 2: epoll -> threadpool
run 3: threadpool -> epoll
run 4: epoll -> threadpool
```

This reduces systematic first-run cache/thermal ordering bias. It does **not** eliminate machine noise or make a noisy workstation a controlled benchmark host.

## Client latency metrics

`stress_http.py` records two latency distributions:

- `attempt_latency`: every measured request attempt, including failures.
- `success_latency`: only requests that returned the expected HTTP status.

The comparison summary uses **successful-request latency** for its primary p50/p95/p99 medians while preserving attempt latency medians as additional evidence. Failure rate must always be reported alongside successful-request latency so a runtime cannot appear fast simply by failing slow requests.

## Server evidence JSON

The benchmark server records:

- runtime (`threadpool` or `epoll`),
- Git revision when available,
- build configuration,
- compiler/version,
- platform,
- benchmark configuration,
- accepted/rejected/completed/failed connections,
- active and peak-active connections,
- thread-pool queued count where applicable,
- process wall duration,
- process CPU seconds,
- Linux peak RSS in KiB where available.

### CPU metric

`process_cpu_seconds` is process CPU time for the benchmark-server process over that run. It is not a system-wide CPU utilization percentage.

### Peak RSS metric

On Linux, `peak_rss_kib` comes from process resource usage and is a high-water mark for that benchmark-server process. Because each orchestrated repetition launches a fresh process, its high-water mark belongs to that individual run.

On platforms where the metric is unavailable, JSON contains `null`; do not substitute a guessed value.

## Required comparison matrix

A publishable local comparison should vary one dimension at a time and include both runtimes under identical client settings.

### Offered concurrency

For a fixed payload/admission/worker count, use values around and above the thread worker count, for example:

```text
1, 2, 4, 8, 16, 32, 64
```

Run both `keepalive` and `connect` when characterizing scheduler and accept behavior.

### Thread-worker sensitivity

For the thread-pool side, repeat otherwise identical comparisons with worker counts such as:

```text
1, 2, 4, 8
```

Keep the total admission budget constant when the goal is runtime comparison.

### Admission pressure

Use a deliberately smaller admission budget and higher connection-churn concurrency. Record client failure rate and server rejection counters. A saturation result is only meaningful when admission capacity, worker count, concurrency, and error policy are all visible.

### Payload sensitivity

Useful starting payloads:

```text
128 B
4 KiB
64 KiB
```

Larger payloads increasingly measure copies, socket buffering, and client limitations rather than only runtime scheduling.

## Repetition and publication rules

For any result that may be committed or cited publicly:

1. Build Release from an identified Git revision.
2. Record CPU model, logical core count, RAM, OS/kernel, compiler/version, and client placement.
3. Record whether the benchmark is same-machine loopback or remote-client load.
4. Keep power mode and major background workload stable.
5. Use the same client scenario for both runtimes.
6. Repeat each runtime/scenario at least five times.
7. Preserve **all** raw runs, including failures and saturation runs.
8. Report medians and representative p50/p95/p99 rather than selecting the best run.
9. Report failure rate and server rejects/failures with throughput/latency.
10. Never combine results from different machines as though directly comparable.
11. Never label loopback results as internet-facing capacity.
12. Explain excluded runs instead of silently deleting them.
13. Do not publish a percentage superiority claim unless the raw repeated evidence and environment controls support it.

## Curated evidence layout

Local scratch output is ignored by Git:

```text
benchmark-results/local/
```

When a controlled measurement set is ready for review, copy the complete selected scenario bundle into a committed directory such as:

```text
benchmark-results/curated/
  2026-09-06-a1b2c3d-linux-machine-name/
    ENVIRONMENT.md
    keepalive-c8/
      summary.json
      threadpool-run-01-client.json
      threadpool-run-01-server.json
      ...
      epoll-run-05-server.log
    connect-c64/
      ...
```

`ENVIRONMENT.md` should record at minimum:

- exact Git SHA,
- clean/dirty working-tree state,
- build command and configuration,
- compiler and version,
- CPU model,
- physical/logical core count when known,
- RAM,
- OS and kernel,
- power/performance mode,
- client placement,
- relevant background workload notes,
- commands used for each scenario,
- reasons for any excluded/invalidated runs.

See [`../benchmark-results/README.md`](../benchmark-results/README.md) for the repository evidence policy.

## CI smoke policy

Linux CI runs a tiny one-repetition `threadpool` + `epoll` comparison only to prove that:

- both benchmark modes launch,
- readiness/control works,
- the client can drive both runtimes,
- raw JSON can be parsed,
- summary generation succeeds,
- both server processes stop cleanly.

The step is explicitly named **not benchmark evidence**. CI does not assert a requests-per-second threshold, latency threshold, or runtime winner. Hosted-runner measurements must not be promoted into repository performance claims merely because the smoke completed.

## Interpretation limits

The first-party Python harness prioritizes transparency and portability over maximum load-generation capacity. At high request rates, Python scheduling, the GIL, loopback networking, client CPU, or `http.client` may become the bottleneck before `vhttp` does.

The Linux `epoll` runtime also still executes application handlers synchronously on the event-loop thread. A slow handler or blocking filesystem operation can stall unrelated event-loop connections. A throughput result from the tiny `/bench` handler therefore does not prove behavior for blocking real-world handlers.

Therefore:

- use the first-party harness for repeatable local comparison/regression evidence;
- use external-host high-rate load generation later for higher-throughput characterization;
- profile server CPU/RSS before attributing a plateau to runtime architecture;
- include blocking-handler scenarios before making broad scalability claims;
- keep correctness evidence and performance evidence as separate gates.

## Current evidence boundary

The repository now contains a complete, reproducible **comparison mechanism** for thread-pool versus Linux `epoll` execution. It intentionally does **not** commit invented benchmark numbers or infer a winner.

The next evidence milestone is to run the documented matrices on a controlled, named Linux machine and commit the complete raw/summary bundle only after review.
