#!/usr/bin/env python3
"""Reproducible vhttp runtime comparison orchestrator.

The tool launches the same benchmark-server binary in each requested runtime
mode, drives it with tools/stress_http.py, preserves every raw client/server
JSON and log, and writes a median summary. It intentionally does not declare a
winner or convert local loopback measurements into universal performance claims.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
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
VALID_RUNTIMES = ("threadpool", "epoll")


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
    execution_order: int
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
        if runtime not in VALID_RUNTIMES:
            raise argparse.ArgumentTypeError(
                "runtimes must contain only threadpool and/or epoll"
            )
        if runtime not in runtimes:
            runtimes.append(runtime)
    if not runtimes:
        raise argparse.ArgumentTypeError("at least one runtime is required")
    return runtimes


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run repeated threadpool/epoll comparisons and preserve raw evidence; "
            "no performance winner is inferred"
        )
    )
    parser.add_argument(
        "--server", default="build/vhttp_bench_server", help="path to vhttp_bench_server"
    )
    parser.add_argument(
        "--client", default="tools/stress_http.py", help="path to stress_http.py"
    )
    parser.add_argument("--output-dir", default="benchmark-results/local/comparison")
    parser.add_argument(
        "--runtimes", type=parse_runtimes, default=parse_runtimes("threadpool,epoll")
    )
    parser.add_argument("--runs", type=positive_int, default=5)
    parser.add_argument("--admission", type=positive_int, default=260)
    parser.add_argument("--workers", type=positive_int, default=4)
    parser.add_argument("--payload", type=positive_int, default=128)
    parser.add_argument("--requests", type=positive_int, default=10000)
    parser.add_argument("--concurrency", type=positive_int, default=8)
    parser.add_argument("--warmup", type=nonnegative_int, default=500)
    parser.add_argument("--mode", choices=("keepalive", "connect"), default="keepalive")
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--max-error-rate", type=rate, default=0.0)
    parser.add_argument("--startup-timeout", type=float, default=10.0)
    parser.add_argument("--client-phase-timeout", type=float, default=180.0)
    parser.add_argument("--shutdown-timeout", type=float, default=15.0)
    parser.add_argument(
        "--require-identified-build",
        action="store_true",
        help="fail if server JSON reports git_revision=unknown",
    )
    args = parser.parse_args()

    if args.timeout <= 0:
        parser.error("--timeout must be greater than zero")
    if args.startup_timeout <= 0:
        parser.error("--startup-timeout must be greater than zero")
    if args.client_phase_timeout <= 0:
        parser.error("--client-phase-timeout must be greater than zero")
    if args.shutdown_timeout <= 0:
        parser.error("--shutdown-timeout must be greater than zero")
    if args.admission <= args.workers and "threadpool" in args.runtimes:
        parser.error("--admission must be greater than --workers for threadpool runs")
    if "epoll" in args.runtimes and platform.system() != "Linux":
        parser.error("epoll comparison is Linux-only; use --runtimes threadpool elsewhere")
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


def nested_mapping(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise RuntimeError(f"expected object for {label}")
    return value


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
            raise RuntimeError(
                f"{runtime} benchmark server did not become ready within {timeout:.1f}s"
            )
        try:
            line = line_queue.get(timeout=min(remaining, 0.25))
        except queue.Empty:
            if process.poll() is not None:
                raise RuntimeError(f"{runtime} benchmark server exited before READY marker")
            continue

        match = READY_PATTERN.match(line)
        if match is None:
            continue
        ready_runtime = match.group(1)
        ready_port = int(match.group(2))
        if ready_runtime != runtime:
            raise RuntimeError(
                f"READY runtime mismatch: expected {runtime}, got {ready_runtime}"
            )
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
    execution_order: int,
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
        completed = subprocess.run(
            client_command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=args.client_phase_timeout,
            check=False,
        )
        client_output = completed.stdout
        client_returncode = completed.returncode
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
    if args.require_identified_build and server.get("git_revision") in (None, "", "unknown"):
        raise RuntimeError(f"server build is not tied to a Git revision in {server_json_path}")

    # Fail early if either evidence schema is incompatible with the summarizer.
    results = nested_mapping(client.get("results"), f"{client_json_path}:results")
    nested_mapping(results.get("attempt_latency"), f"{client_json_path}:attempt_latency")
    nested_mapping(results.get("success_latency"), f"{client_json_path}:success_latency")
    nested_mapping(server.get("runtime_stats"), f"{server_json_path}:runtime_stats")
    nested_mapping(server.get("resources"), f"{server_json_path}:resources")

    return RunEvidence(
        runtime=runtime,
        run_index=run_index,
        execution_order=execution_order,
        client_json_path=client_json_path,
        server_json_path=server_json_path,
        client_log_path=client_log_path,
        server_log_path=server_log_path,
        client=client,
        server=server,
    )


def runtime_summary(evidence: list[RunEvidence]) -> dict[str, Any]:
    client_results = [nested_mapping(item.client["results"], "client results") for item in evidence]
    success_latencies = [
        nested_mapping(result["success_latency"], "success latency") for result in client_results
    ]
    attempt_latencies = [
        nested_mapping(result["attempt_latency"], "attempt latency") for result in client_results
    ]
    server_stats = [
        nested_mapping(item.server["runtime_stats"], "runtime stats") for item in evidence
    ]
    resources = [nested_mapping(item.server["resources"], "resources") for item in evidence]

    return {
        "runs": len(evidence),
        "latency_basis": "success_latency",
        "git_revisions": sorted(
            {str(item.server.get("git_revision", "unknown")) for item in evidence}
        ),
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
            "failure_rate": median_optional(
                [result.get("failure_rate") for result in client_results]
            ),
            "success_p50_ms": median_optional(
                [latency.get("p50_ms") for latency in success_latencies]
            ),
            "success_p95_ms": median_optional(
                [latency.get("p95_ms") for latency in success_latencies]
            ),
            "success_p99_ms": median_optional(
                [latency.get("p99_ms") for latency in success_latencies]
            ),
            "attempt_p50_ms": median_optional(
                [latency.get("p50_ms") for latency in attempt_latencies]
            ),
            "attempt_p95_ms": median_optional(
                [latency.get("p95_ms") for latency in attempt_latencies]
            ),
            "attempt_p99_ms": median_optional(
                [latency.get("p99_ms") for latency in attempt_latencies]
            ),
            "process_cpu_seconds": median_optional(
                [resource.get("process_cpu_seconds") for resource in resources]
            ),
            "wall_seconds": median_optional([resource.get("wall_seconds") for resource in resources]),
            "peak_rss_kib": median_optional(
                [resource.get("peak_rss_kib") for resource in resources]
            ),
            "peak_active_connections": median_optional(
                [stats.get("peak_active") for stats in server_stats]
            ),
            "rejected_connections": median_optional(
                [stats.get("rejected") for stats in server_stats]
            ),
            "failed_connections": median_optional(
                [stats.get("failed") for stats in server_stats]
            ),
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
        "schema_version": 2,
        "generated_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "interpretation": (
            "Raw local measurements only. This summary intentionally does not declare a winning "
            "runtime or a universal capacity/performance claim."
        ),
        "ordering_policy": (
            "Runtime order alternates by repetition when more than one runtime is selected, reducing "
            "systematic first-run thermal/cache bias without pretending to eliminate it."
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
            "logical_cpu_count": os.cpu_count(),
        },
        "runtime_summaries": {
            runtime: runtime_summary(items) for runtime, items in sorted(grouped.items())
        },
        "raw_runs": [
            {
                "execution_order": item.execution_order,
                "runtime": item.runtime,
                "run_index": item.run_index,
                "client_json": item.client_json_path.name,
                "server_json": item.server_json_path.name,
                "client_log": item.client_log_path.name,
                "server_log": item.server_log_path.name,
            }
            for item in sorted(all_evidence, key=lambda run: run.execution_order)
        ],
    }
    path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return summary


def format_number(value: float | None, decimals: int) -> str:
    return "n/a" if value is None else f"{value:.{decimals}f}"


def print_summary(summary: dict[str, Any]) -> None:
    print("vhttp runtime comparison summary")
    runtime_summaries = nested_mapping(summary["runtime_summaries"], "runtime summaries")
    for runtime, data_value in runtime_summaries.items():
        data = nested_mapping(data_value, f"summary for {runtime}")
        median = nested_mapping(data["median"], f"median for {runtime}")
        print(f"  {runtime} ({data['runs']} run(s))")
        print(
            "    median requests/s: "
            + format_number(median.get("requests_per_second"), 2)
        )
        print(
            "    median success p50/p95/p99 ms: "
            + " / ".join(
                [
                    format_number(median.get("success_p50_ms"), 3),
                    format_number(median.get("success_p95_ms"), 3),
                    format_number(median.get("success_p99_ms"), 3),
                ]
            )
        )
        failure_rate = median.get("failure_rate")
        failure_text = "n/a" if failure_rate is None else f"{float(failure_rate) * 100.0:.3f}%"
        print(f"    median failure rate: {failure_text}")
        print(
            "    median server CPU seconds: "
            + format_number(median.get("process_cpu_seconds"), 6)
        )
        print(
            "    median peak RSS KiB: " + format_number(median.get("peak_rss_kib"), 0)
        )
        print(
            "    median peak active connections: "
            + format_number(median.get("peak_active_connections"), 0)
        )
    print("  No winner is inferred; inspect raw evidence and environment metadata.")


def execution_plan(runtimes: list[str], runs: int) -> list[tuple[str, int]]:
    plan: list[tuple[str, int]] = []
    for run_index in range(1, runs + 1):
        ordered = runtimes if run_index % 2 == 1 else list(reversed(runtimes))
        for runtime in ordered:
            plan.append((runtime, run_index))
    return plan


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
    plan = execution_plan(args.runtimes, args.runs)
    for execution_order, (runtime, run_index) in enumerate(plan, start=1):
        print(
            f"running {runtime} repetition {run_index}/{args.runs} "
            f"(execution {execution_order}/{len(plan)})...",
            flush=True,
        )
        evidence.append(
            run_once(
                args,
                scenario,
                runtime,
                run_index,
                execution_order,
                output_dir,
            )
        )

    summary_path = output_dir / "summary.json"
    summary = write_summary(summary_path, args, scenario, evidence)
    print_summary(summary)
    print(f"  summary JSON: {summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
