# Release Readiness — v0.1.0

This checklist defines when `vhttp` may be tagged as a **C++20 HTTP/1.1 engineering reference release**. A release may document implemented protocol/runtime behavior without claiming production readiness or universal performance superiority.

## Allowed release statement

> **v0.1.0 — Cross-platform C++20 HTTP/1.1 engineering server with shared blocking/thread-pool and Linux epoll semantics, adversarial verification, and reproducible benchmark tooling. Controlled-host performance evidence remains separate.**

## Required gates

### Build and test matrix

- [ ] Linux GCC CI is green on the exact tag commit.
- [ ] Linux Clang CI is green on the exact tag commit.
- [ ] Windows MSVC CI is green on the exact tag commit.
- [ ] Parser, routing, framing, static-file, lifecycle, thread-pool, and epoll tests pass.
- [ ] Real loopback TCP tests pass on supported CI hosts.
- [ ] Release build succeeds with documented CMake commands.

### Protocol/security behavior

- [ ] Request-size/header/body limits remain explicit.
- [ ] `Transfer-Encoding`/`Content-Length` ambiguity remains rejected.
- [ ] HTTP persistence/pipelining behavior matches documented tests.
- [ ] Static-file traversal, encoded-separator, symlink/reparse, range, validator, and `HEAD` behavior remain covered.
- [ ] Known filesystem race limitations remain documented.
- [ ] Event-loop handler blocking limitations remain documented.

### Benchmark integrity

- [ ] CI smoke results are not presented as benchmark evidence.
- [ ] No threadpool-vs-epoll winner is declared without reviewed controlled-host bundles.
- [ ] `tools/compare_runtimes.py` and M6B.2 campaign tooling preserve raw client/server JSON and logs.
- [ ] Any performance statement names host, kernel, compiler, build type, exact Git revision, scenario, run count, and failure distribution.
- [ ] Python load-generator bottleneck risk remains stated for high-rate tests.

### Release package

- [ ] README build/run examples match the tagged tree.
- [ ] `docs/ARCHITECTURE.md`, `docs/SECURITY.md`, and `docs/PERFORMANCE.md` match implementation behavior.
- [ ] MIT license is present.
- [ ] Release notes explicitly state that this is not a production-ready reverse proxy.
- [ ] Release notes list major current limitations.
- [ ] No local benchmark scratch data is accidentally included.

## M6B.2 evidence gate

A later evidence-bearing release may include measured runtime comparisons only after a controlled Linux campaign provides:

- named host CPU/RAM/kernel;
- compiler and Release-build configuration;
- exact Git revision;
- alternating threadpool/epoll run order;
- repeated keep-alive and connection-churn matrices;
- raw server/client JSON and logs;
- throughput and successful-request p50/p95/p99;
- failure/status distributions;
- CPU and peak RSS;
- load-generator bottleneck review;
- constrained, scenario-specific conclusions.

The absence of M6B.2 evidence does not block a software-reference `v0.1.0`; it blocks measured comparative-performance claims.

## Recommended first release title

`v0.1.0 — C++20 HTTP/1.1 Server Engineering Reference`

## Recruiter review path

1. `README.md` — implemented HTTP/runtime surface and limitations;
2. `docs/ARCHITECTURE.md` — shared connection/session design;
3. `docs/SECURITY.md` — request/static-file threat model;
4. `docs/PERFORMANCE.md` — benchmark methodology;
5. `tools/compare_runtimes.py` + M6B.2 tooling — reproducibility;
6. Linux/Windows workflows — cross-platform verification;
7. this checklist — release and claim boundaries.
