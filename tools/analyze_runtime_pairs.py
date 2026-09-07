#!/usr/bin/env python3
"""Analyze paired threadpool/epoll benchmark repetitions without universal claims."""
from __future__ import annotations

import argparse
import json
import pathlib
import statistics
from typing import Any


def load_json(path: pathlib.Path) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError(f"expected JSON object: {path}")
    return data


def pct_change(new: float, baseline: float) -> float:
    if baseline == 0:
        raise ValueError("baseline must be non-zero for percent change")
    return (new - baseline) / baseline * 100.0


def client_metrics(client: dict[str, Any]) -> dict[str, float]:
    results = client.get("results")
    if not isinstance(results, dict):
        raise ValueError("client JSON missing results object")
    success_latency = results.get("success_latency")
    if not isinstance(success_latency, dict):
        raise ValueError("client JSON missing success_latency object")
    return {
        "successful_requests_per_second": float(results["successful_requests_per_second"]),
        "failure_rate": float(results["failure_rate"]),
        "success_p95_ms": float(success_latency["p95_ms"]),
        "success_p99_ms": float(success_latency["p99_ms"]),
    }


def analyze(directory: pathlib.Path) -> dict[str, Any]:
    summary = load_json(directory / "summary.json")
    raw_runs = summary.get("raw_runs")
    if not isinstance(raw_runs, list):
        raise ValueError("summary.json missing raw_runs list")

    by_index: dict[int, dict[str, dict[str, float]]] = {}
    for item in raw_runs:
        if not isinstance(item, dict):
            raise ValueError("raw_runs entries must be objects")
        runtime = str(item.get("runtime"))
        if runtime not in {"threadpool", "epoll"}:
            continue
        run_index = int(item["run_index"])
        client_path = directory / str(item["client_json"])
        by_index.setdefault(run_index, {})[runtime] = client_metrics(load_json(client_path))

    paired: list[dict[str, Any]] = []
    for run_index in sorted(by_index):
        pair = by_index[run_index]
        if set(pair) != {"threadpool", "epoll"}:
            raise ValueError(f"run {run_index} is not a complete threadpool/epoll pair")
        threadpool = pair["threadpool"]
        epoll = pair["epoll"]
        paired.append(
            {
                "run_index": run_index,
                "epoll_vs_threadpool_percent": {
                    "successful_requests_per_second": pct_change(
                        epoll["successful_requests_per_second"],
                        threadpool["successful_requests_per_second"],
                    ),
                    "success_p95_ms": pct_change(epoll["success_p95_ms"], threadpool["success_p95_ms"]),
                    "success_p99_ms": pct_change(epoll["success_p99_ms"], threadpool["success_p99_ms"]),
                    "failure_rate": (
                        pct_change(epoll["failure_rate"], threadpool["failure_rate"])
                        if threadpool["failure_rate"] != 0
                        else None
                    ),
                },
                "threadpool": threadpool,
                "epoll": epoll,
            }
        )

    if not paired:
        raise ValueError("no complete threadpool/epoll pairs found")

    def deltas(metric: str) -> list[float]:
        return [float(item["epoll_vs_threadpool_percent"][metric]) for item in paired]

    throughput = deltas("successful_requests_per_second")
    p95 = deltas("success_p95_ms")
    p99 = deltas("success_p99_ms")

    return {
        "schema_version": 1,
        "evidence_type": "controlled_host_paired_benchmark_analysis",
        "scope": "documented host and scenario only",
        "interpretation": (
            "Positive throughput delta means epoll measured higher; negative latency delta means epoll measured lower. "
            "This analysis summarizes paired repetitions and does not establish a universal runtime winner."
        ),
        "pairs": len(paired),
        "median_epoll_vs_threadpool_percent": {
            "successful_requests_per_second": statistics.median(throughput),
            "success_p95_ms": statistics.median(p95),
            "success_p99_ms": statistics.median(p99),
        },
        "direction_consistency": {
            "throughput_epoll_higher_pairs": sum(value > 0 for value in throughput),
            "p95_epoll_lower_pairs": sum(value < 0 for value in p95),
            "p99_epoll_lower_pairs": sum(value < 0 for value in p99),
        },
        "paired_runs": paired,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("comparison_dir", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    report = analyze(args.comparison_dir)
    text = json.dumps(report, indent=2, sort_keys=True)
    print(text)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
