#!/usr/bin/env python3
"""Run the M6B.2 controlled-host comparison as one reproducible Linux evidence job.

The script refuses non-Linux hosts and dirty trees by default, builds Release once,
runs the documented keep-alive and connection-churn matrices, and records host/build
metadata beside the raw evidence. Output remains local/unreviewed until manually curated.
"""

from __future__ import annotations

import argparse
import datetime as dt
import os
import pathlib
import platform
import shlex
import socket
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]


def run_capture(command: list[str]) -> str:
    completed = subprocess.run(command, cwd=ROOT, check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return completed.stdout.strip()


def run(command: list[str]) -> None:
    print("+", shlex.join(command), flush=True)
    subprocess.run(command, cwd=ROOT, check=True)


def read_first(path: pathlib.Path, default: str = "unknown") -> str:
    try:
        return path.read_text(encoding="utf-8").strip()
    except OSError:
        return default


def cpu_model() -> str:
    try:
        for line in pathlib.Path("/proc/cpuinfo").read_text(encoding="utf-8").splitlines():
            if line.lower().startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def ram_total() -> str:
    try:
        for line in pathlib.Path("/proc/meminfo").read_text(encoding="utf-8").splitlines():
            if line.startswith("MemTotal:"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return "unknown"


def os_release() -> str:
    try:
        values = {}
        for line in pathlib.Path("/etc/os-release").read_text(encoding="utf-8").splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                values[key] = value.strip().strip('"')
        return values.get("PRETTY_NAME", "unknown")
    except OSError:
        return "unknown"


def scenario_command(server: pathlib.Path, output: pathlib.Path, mode: str, concurrency: int, runs: int, requests: int, warmup: int) -> list[str]:
    return [
        sys.executable,
        str(ROOT / "tools" / "compare_runtimes.py"),
        "--server", str(server),
        "--output-dir", str(output),
        "--runtimes", "threadpool,epoll",
        "--runs", str(runs),
        "--admission", "260",
        "--workers", "4",
        "--payload", "128",
        "--requests", str(requests),
        "--concurrency", str(concurrency),
        "--warmup", str(warmup),
        "--mode", mode,
        "--require-identified-build",
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=pathlib.Path, default=ROOT / "build-m6b2")
    parser.add_argument("--output-root", type=pathlib.Path, default=ROOT / "benchmark-results" / "local")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--requests", type=int, default=10000)
    parser.add_argument("--warmup", type=int, default=500)
    parser.add_argument("--allow-dirty", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    if platform.system() != "Linux":
        print("M6B.2 controlled evidence requires Linux because the epoll runtime is Linux-only.")
        return 2
    if args.runs < 5:
        parser.error("--runs must be >= 5 for curated M6B.2 evidence")
    if args.requests <= 0 or args.warmup < 0:
        parser.error("invalid request/warmup counts")

    sha = run_capture(["git", "rev-parse", "HEAD"])
    short_sha = sha[:8]
    dirty = bool(run_capture(["git", "status", "--porcelain"]))
    if dirty and not args.allow_dirty:
        print("Refusing controlled benchmark from a dirty working tree. Commit/stash changes or use --allow-dirty for non-curated exploration.")
        return 3

    hostname = socket.gethostname().replace(" ", "-")
    stamp = dt.datetime.now(dt.timezone.utc).date().isoformat()
    bundle = args.output_root / f"{stamp}-{short_sha}-{hostname}"
    keepalive = bundle / "keepalive-c8"
    connect = bundle / "connect-c64"
    server = args.build_dir / "vhttp_bench_server"

    configure = ["cmake", "-S", ".", "-B", str(args.build_dir), "-DCMAKE_BUILD_TYPE=Release"]
    build = ["cmake", "--build", str(args.build_dir), "--target", "vhttp_bench_server", "--parallel"]
    keepalive_cmd = scenario_command(server, keepalive, "keepalive", 8, args.runs, args.requests, args.warmup)
    connect_cmd = scenario_command(server, connect, "connect", 64, args.runs, args.requests, args.warmup)

    if args.dry_run:
        for command in (configure, build, keepalive_cmd, connect_cmd):
            print(shlex.join(command))
        return 0

    bundle.mkdir(parents=True, exist_ok=False)
    run(configure)
    run(build)

    compiler = run_capture(["c++", "--version"]).splitlines()[0]
    environment = f"""# M6B.2 Controlled Host Environment

- Git SHA: `{sha}`
- Working tree clean: `{not dirty}`
- Hostname: `{hostname}`
- OS: `{os_release()}`
- Kernel: `{platform.release()}`
- Architecture: `{platform.machine()}`
- CPU: `{cpu_model()}`
- Logical CPUs: `{os.cpu_count() or 'unknown'}`
- RAM: `{ram_total()}`
- Compiler: `{compiler}`
- Build configuration: `Release`
- Client location: `same host / loopback`
- Background workload: `operator must document before curation`
- Power/performance mode: `operator must document before curation`

## Build commands

```bash
{shlex.join(configure)}
{shlex.join(build)}
```

## Comparison commands

```bash
{shlex.join(keepalive_cmd)}
{shlex.join(connect_cmd)}
```

## Scope

This bundle is machine- and workload-specific. It does not establish universal threadpool/epoll superiority and must remain under `benchmark-results/local/` until reviewed.
"""
    (bundle / "ENVIRONMENT.md").write_text(environment, encoding="utf-8")

    run(keepalive_cmd)
    run(connect_cmd)

    validator = [sys.executable, str(ROOT / "tools" / "validate_m6b2_bundle.py"), str(bundle)]
    run(validator)
    print(f"M6B.2 local evidence bundle complete: {bundle}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
