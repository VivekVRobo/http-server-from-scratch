# Controlled-Host Benchmark Environment

> Copy this file into `benchmark-results/curated/<run-id>/ENVIRONMENT.md` for a real M6B.2 evidence bundle. Hosted CI smoke numbers are not benchmark evidence.

## Run identity

- Run ID:
- Date/time + timezone:
- Operator:
- Git commit SHA:
- Working tree clean: yes / no
- Machine name / identifier:

## Host

- OS distribution / version:
- Kernel:
- CPU model:
- Physical cores:
- Logical CPUs:
- RAM:
- Storage relevant to build/run:
- Governor / power mode:
- Virtualized: yes / no
- Thermal / cooling notes:
- Background workload notes:

## Toolchain

- Compiler:
- Compiler version:
- CMake version:
- Python version:
- Build type: Release
- Build command:

```text
<exact command>
```

## Benchmark topology

- Server host:
- Client host:
- Same machine: yes / no
- Network path if remote:
- Loopback used: yes / no

## Server configuration

- Runtime candidates: `threadpool`, `epoll`
- Admission budget:
- Thread-pool workers:
- Payload bytes:
- Other server flags:

## Scenario A — keep-alive

Command:

```text
python3 tools/compare_runtimes.py \
  --server <path> \
  --output-dir <path> \
  --runtimes threadpool,epoll \
  --runs 5 \
  --admission <n> \
  --workers <n> \
  --payload <bytes> \
  --requests <n> \
  --concurrency <n> \
  --warmup <n> \
  --mode keepalive \
  --require-identified-build
```

Actual command used:

```text
<exact command>
```

## Scenario B — connection churn

Actual command used:

```text
<exact command>
```

## Raw evidence audit

For every retained repetition:

- [ ] client JSON exists
- [ ] server JSON exists
- [ ] client log exists
- [ ] server log exists
- [ ] runtime order preserved
- [ ] failed/saturated runs were not silently removed
- [ ] summary JSON is reproducible from retained raw files

## Load-generator sanity check

Describe how you checked whether `tools/stress_http.py` was an obvious bottleneck at the reported request rate/concurrency. If inconclusive, state that limitation instead of claiming server saturation.

## Results summary

Report medians across repeated runs, not the single best run.

| Scenario | Runtime | Throughput | Success p50 | Success p95 | Success p99 | Failure/rejection rate | CPU | Peak RSS |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| keepalive | threadpool | | | | | | | |
| keepalive | epoll | | | | | | | |
| connect | threadpool | | | | | | | |
| connect | epoll | | | | | | | |

## Invalidated / excluded runs

List every excluded run and the reason. Never remove an inconvenient run without recording why it is invalid.

## Constrained conclusion

Use wording limited to this exact host/build/scenario. Example structure:

> On `<machine>` at commit `<sha>`, under `<scenario/configuration>`, `<runtime>` measured `<specific difference>`. This result is limited to the documented workload and does not establish universal superiority or production readiness.

## Claim gate

- [ ] Exact commit/build identified
- [ ] Host fully documented
- [ ] At least five runs per runtime/scenario retained
- [ ] Runtime order alternated
- [ ] Raw client/server evidence retained
- [ ] Failures/rejections reported
- [ ] p50/p95/p99 successful-request latency reported
- [ ] CPU and peak RSS recorded
- [ ] Client bottleneck considered
- [ ] Conclusion limited to measured conditions

## Recruiter review path

`commit -> host/toolchain -> build -> exact commands -> raw runs -> summaries -> resource data -> invalidations -> constrained conclusion`
