#!/usr/bin/env python3
"""Run reproducible vhttp runtime comparisons without inventing conclusions.

This orchestrator launches the same benchmark server binary in each requested
runtime mode, drives it with tools/stress_http.py, preserves raw client/server
JSON plus logs for every run, and writes a median summary. It intentionally does
not label a runtime as a winner; interpretation belongs with the raw evidence.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import pathlib
import platform
import queue
import re
import socket
import statistics
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from typing import Any


READY_PATTERN = re.compile(r"^READY runtime=(threadpool|epoll) port=(\d+)$")


@dataclass(frozen=True)
class Scenario:
    admission: int
    workers: int
    payload: int
    requests: int
    concurrency: int
    warmup: int
    mode: str
    timeout: float
    max_error_rate: float


@dataclass
class RunEvidence:
    runtime: str
    run_index: int
    client_json_path: pathlib.Path
    server_json_path: pathlib.Path
    client_log_path: pathlib.Path
    server_log_path: pathlib.Path
    client: dict[str, Any]
    server: dict[str, Any]


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return parsed


def nonnegative_int(value: str) -> int:
    parsed = int(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("must be a non-negative integer")
    return parsed


def rate(value: str) -> float:
    parsed = float(value)
    if not 0.0 <= parsed <= 1.0:
        raise argparse.ArgumentTypeError("must be between 0 and 1")
    return parsed


def parse_runtimes(value: str) -> list[str]:
    runtimes: list[str] = []
    for item in value.split(","):
        runtime = item.strip().lower()
        if not runtime:
            continue
        if runtime not in {"threadpool", "epoll"}:
            raise argparse.ArgumentTypeError("runtimes must contain only threadpool and/or epoll")
        if runtime not in runtimes:
            runtimes.append(runtime)
    if not runtimes:
        raise argparse.ArgumentTypeError("at least one runtime is required")
    return runtimes


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run repeatable threadpool/epoll benchmark comparisons and preserve raw evidence"
    )
    parser.add_argument("--server", default="build/vhttp_bench_server", help="path to vhttp_bench_server")
    parser.add_argument("--client", default="tools/stress_http.py", help="path to stress_http.py")
    parser.add_argument("--output-dir", default="benchmark-results/local/comparison")
    parser.add_argument("--runtimes", type=parse_runtimes, default=parse_runtimes("threadpool,epoll"))
    parser.add_argument("--runs", type=positive_int, default=5)
    parser.add_argument("--admission", type=positive_int, default=260)
    parser.add_argument("--workers", type=positive_int, default=4)
    parser.add_argument("--payload", type=positive_int, default=128)
    parser.add_argument("--requests", type=positive_int, default=10000)
    parser.add_argument("--concurrency", type=positive_int, default=8)
    parser.add_argument("--warmup", type=nonnegative_int, default=500)
    parser.add_argument("--mode", choices=("keepalive", "connect"), default="keepalive")
    parser.add_argument("--timeout", type=float, default=5.0, help="per-request socket timeout seconds")
    parser.add_argument("--max-error-rate", type=rate, default=0.0)
    parser.add_argument("--startup-timeout", type=float, default=10.0)
    parser.add_argument("--client-phase-timeout", type=float, default=180.0)
    parser.add_argument("--shutdown-timeout", type=float, default=15.0)
    parser.add_argument(
        "--require-identified-build",
        action="store_true",
        help="fail if the benchmark server reports git_revision=unknown",
    )
    args = parser.parse_args()

    if args.timeout <= 0:
        parser.error("--timeout must be greater than zero")
    if args.startup_timeout <= 0 or args.client_phase_timeout <= 0 or args.shutdown_timeout <= 0:
        parser.error("phase timeout values must be greater than zero")
    if args.admission <= args.workers and "threadpool" in args.runtimes:
        parser.error("--admission must be greater than --workers for threadpool comparisons")
    if "epoll" in args.runtimes and platform.system() != "Linux":
        parser.error("epoll comparison is supported only on Linux; use --runtimes threadpool elsewhere")
    return args


def choose_loopback_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def load_json(path: pathlib.Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        data = json.load(handle)
    if not isinstance(data, dict):
        raise RuntimeError(f"expected JSON object in {path}")
    return data


def median_optional(values: list[float | int | None]) -> float | None:
    present = [float(value) for value in values if value is not None]
    return statistics.median(present) if present else None


def read_stream(stream: Any, line_queue: queue.Queue[str], lines: list[str]) -> None:
    try:
        for line in iter(stream.readline, ""):
            lines.append(line)
            line_queue.put(line.rstrip("\r\n"))
    finally:
        stream.close()


def wait_for_ready(
    process: subprocess.Popen[str],
    line_queue: queue.Queue[str],
    runtime: str,
    expected_port: int,
    timeout: float,
) -> None:
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise RuntimeError(f"{runtime} benchmark server did not become ready within {timeout:.1f}s")
        try:
            line = line_queue.get(timeout=min(remaining, 0.25))
        except queue.Empty:
            if process.poll() is not None:
                raise RuntimeError(f"{runtime} benchmark server exited before READY marker")
            continue

        match = READY_PATTERN.match(line)
        if not match:
            continue
        ready_runtime = match.group(1)
        ready_port = int(match.group(2))
        if ready_runtime != runtime:
            raise RuntimeError(f"READY runtime mismatch: expected {runtime}, got {ready_runtime}")
        if ready_port != expected_port:
            raise RuntimeError(f"READY port mismatch: expected {expected_port}, got {ready_port}")
        return


def stop_server(process: subprocess.Popen[str], shutdown_timeout: float) -> None:
    if process.poll() is not None:
        return
    if process.stdin is None:
        raise RuntimeError("benchmark server stdin pipe is unavailable")
    try:
        process.stdin.write("stop\n")
        process.stdin.flush()
        process.stdin.close()
    except (BrokenPipeError, OSError):
        pass

    try:
        process.wait(timeout=shutdown_timeout)
    except subprocess.TimeoutExpired as exc:
        process.terminate()
        try:
            process.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5.0)
        raise RuntimeError("benchmark server did not drain within shutdown timeout") from exc


def run_once(
    args: argparse.Namespace,
    scenario: Scenario,
    runtime: str,
    run_index: int,
    output_dir: pathlib.Path,
) -> RunEvidence:
    port = choose_loopback_port()
    prefix = f"{runtime}-run-{run_index:02d}"
    client_json_path = output_dir / f"{prefix}-client.json"
    server_json_path = output_dir / f"{prefix}-server.json"
    client_log_path = output_dir / f"{prefix}-client.log"
    server_log_path = output_dir / f"{prefix}-server.log"

    server_command = [
        str(args.server),
        "--runtime",
        runtime,
        "--port",
        str(port),
        "--admission",
        str(scenario.admission),
        "--payload",
        str(scenario.payload),
        "--until-stdin",
        "--stats-json",
        str(server_json_path),
    ]
    if runtime == "threadpool":
        server_command.extend(["--workers", str(scenario.workers)])

    server_lines: list[str] = []
    line_queue: queue.Queue[str] = queue.Queue()
    server_process = subprocess.Popen(
        server_command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    if server_process.stdout is None:
        server_process.kill()
        raise RuntimeError("benchmark server stdout pipe is unavailable")
    reader = threading.Thread(
        target=read_stream,
        args=(server_process.stdout, line_queue, server_lines),
        daemon=True,
    )
    reader.start()

    client_output = ""
    client_returncode: int | None = None
    try:
        wait_for_ready(server_process, line_queue, runtime, port, args.startup_timeout)
        client_command = [
            sys.executable,
            str(args.client),
            "--host",
            "127.0.0.1",
            "--port",
            str(port),
            "--path",
            "/bench",
            "--requests",
            str(scenario.requests),
            "--concurrency",
            str(scenario.concurrency),
            "--warmup",
            str(scenario.warmup),
            "--mode",
            scenario.mode,
            "--timeout",
            str(scenario.timeout),
            "--max-error-rate",
            str(scenario.max_error_rate),
            "--json-out",
            str(client_json_path),
        ]
        client_completed = subprocess.run(
            client_command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=args.client_phase_timeout,
            check=False,
        )
        client_output = client_completed.stdout
        client_returncode = client_completed.returncode
    finally:
        try:
            stop_server(server_process, args.shutdown_timeout)
        finally:
            reader.join(timeout=2.0)
            server_log_path.write_text("".join(server_lines), encoding="utf-8")
            client_log_path.write_text(client_output, encoding="utf-8")

    if client_returncode is None:
        raise RuntimeError(f"{runtime} client phase did not complete")
    if client_returncode != 0:
        raise RuntimeError(
            f"{runtime} client phase exited {client_returncode}; see {client_log_path}"
        )
    if server_process.returncode != 0:
        raise RuntimeError(
            f"{runtime} benchmark server exited {server_process.returncode}; see {server_log_path}"
        )
    if not client_json_path.exists():
        raise RuntimeError(f"missing client JSON evidence: {client_json_path}")
    if not server_json_path.exists():
        raise RuntimeError(f"missing server JSON evidence: {server_json_path}")

    client = load_json(client_json_path)
    server = load_json(server_json_path)
    if server.get("runtime") != runtime:
        raise RuntimeError(f"server JSON runtime mismatch in {server_json_path}")
    if args.require_identified_build and server.get("git_revision") in {None, "", "unknown"}:
        raise RuntimeError(f"server build is not tied to a Git revision in {server_json_path}")

    return RunEvidence(
        runtime=runtime,
        run_index=run_index,
        client_json_path=client_json_path,
        server_json_path=server_json_path,
        client_log_path=client_log_path,
        server_log_path=server_log_path,
        client=client,
        server=server,
    )


def runtime_summary(evidence: list[RunEvidence]) -> dict[str, Any]:
    client_results = [item.client["results"] for item in evidence]
    server_stats = [item.server["runtime_stats"] for item in evidence]
    resources = [item.server["resources"] for item in evidence]

    return {
        "runs": len(evidence),
        "git_revisions": sorted({str(item.server.get("git_revision", "unknown")) for item in evidence}),
        "compilers": sorted({str(item.server.get("compiler", "unknown")) for item in evidence}),
        "build_configurations": sorted(
            {str(item.server.get("build_configuration", "unknown")) for item in evidence}
        ),
        "median": {
            "requests_per_second": median_optional(
                [result.get("requests_per_second") for result in client_results]
            ),
            "successful_requests_per_second": median_optional(
                [result.get("successful_requests_per_second") for result in client_results]
            ),
            "failure_rate": median_optional([result.get("failure_rate") for result in client_results]),
            "p50_ms": median_optional([result["latency"].get("p50_ms") for result in client_results]),
            "p95_ms": median_optional([result["latency"].get("p95_ms") for result in client_results]),
            "p99_ms": median_optional([result["latency"].get("p99_ms") for result in client_results]),
            "process_cpu_seconds": median_optional(
                [resource.get("process_cpu_seconds") for resource in resources]
            ),
            "peak_rss_kib": median_optional([resource.get("peak_rss_kib") for resource in resources]),
            "peak_active_connections": median_optional(
                [stats.get("peak_active") for stats in server_stats]
            ),
            "rejected_connections": median_optional([stats.get("rejected") for stats in server_stats]),
            "failed_connections": median_optional([stats.get("failed") for stats in server_stats]),
        },
    }


def write_summary(
    path: pathlib.Path,
    args: argparse.Namespace,
    scenario: Scenario,
    all_evidence: list[RunEvidence],
) -> dict[str, Any]:
    grouped: dict[str, list[RunEvidence]] = {}
    for item in all_evidence:
        grouped.setdefault(item.runtime, []).append(item)

    summary: dict[str, Any] = {
        "schema_version": 1,
        "generated_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "interpretation": (
            "Raw local measurements only. This summary intentionally does not declare a winning runtime "
            "or a universal capacity/performance claim."
        ),
        "scenario": {
            "admission_capacity": scenario.admission,
            "threadpool_workers": scenario.workers,
            "payload_bytes": scenario.payload,
            "requests": scenario.requests,
            "concurrency": scenario.concurrency,
            "warmup_requests": scenario.warmup,
            "client_mode": scenario.mode,
            "client_timeout_seconds": scenario.timeout,
            "max_error_rate": scenario.max_error_rate,
            "runs_per_runtime": args.runs,
        },
        "orchestrator_environment": {
            "platform": platform.platform(),
            "python": platform.python_version(),
            "logical_cpu_count": __import__("os").cpu_count(),
        },
        "runtime_summaries": {
            runtime: runtime_summary(items) for runtime, items in sorted(grouped.items())
        },
        "raw_runs": [
            {
                "runtime": item.runtime,
                "run_index": item.run_index,
                "client_json": item.client_json_path.name,
                "server_json": item.server_json_path.name,
                "client_log": item.client_log_path.name,
                "server_log": item.server_log_path.name,
            }
            for item in all_evidence
        ],
    }
    path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return summary


def print_summary(summary: dict[str, Any]) -> None:
    print("vhttp runtime comparison summary")
    for runtime, data in summary["runtime_summaries"].items():
        median = data["median"]
        print(f"  {runtime} ({data['runs']} run(s))")
        print(f"    median requests/s: {median['requests_per_second']:.2f}")
        print(f"    median p50/p95/p99 ms: {median['p50_ms']:.3f} / {median['p95_ms']:.3f} / {median['p99_ms']:.3f}")
        print(f"    median failure rate: {median['failure_rate'] * 100.0:.3f}%")
        print(f"    median server CPU seconds: {median['process_cpu_seconds']:.6f}")
        rss = median["peak_rss_kib"]
        print(f"    median peak RSS KiB: {'n/a' if rss is None else f'{rss:.0f}'}")
        print(f"    median peak active connections: {median['peak_active_connections']:.0f}")
    print("  No winner is inferred; inspect raw evidence and environment metadata.")


def main() -> int:
    args = parse_args()
    output_dir = pathlib.Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    server_path = pathlib.Path(args.server)
    client_path = pathlib.Path(args.client)
    if not server_path.exists():
        raise SystemExit(f"benchmark server not found: {server_path}")
    if not client_path.exists():
        raise SystemExit(f"stress client not found: {client_path}")

    scenario = Scenario(
        admission=args.admission,
        workers=args.workers,
        payload=args.payload,
        requests=args.requests,
        concurrency=args.concurrency,
        warmup=args.warmup,
        mode=args.mode,
        timeout=args.timeout,
        max_error_rate=args.max_error_rate,
    )

    evidence: list[RunEvidence] = []
    for runtime in args.runtimes:
        for run_index in range(1, args.runs + 1):
            print(f"running {runtime} evidence {run_index}/{args.runs}...", flush=True)
            evidence.append(run_once(args, scenario, runtime, run_index, output_dir))

    summary_path = output_dir / "summary.json"
    summary = write_summary(summary_path, args, scenario, evidence)
    print_summary(summary)
    print(f"  summary JSON: {summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
