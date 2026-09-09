#!/usr/bin/env python3
"""Validate structural completeness of an M6B.2 controlled-host evidence bundle."""

from __future__ import annotations

import argparse
import json
import pathlib
import re

EXPECTED = {
    "keepalive-c8": ("keepalive", 8),
    "connect-c64": ("connect", 64),
}


def load_json(path: pathlib.Path) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError(f"expected JSON object: {path}")
    return data


def validate(bundle: pathlib.Path) -> list[str]:
    errors: list[str] = []
    env_path = bundle / "ENVIRONMENT.md"
    if not env_path.is_file():
        return ["missing ENVIRONMENT.md"]
    env_text = env_path.read_text(encoding="utf-8")
    sha_match = re.search(r"Git SHA: `([0-9a-f]{40})`", env_text)
    if not sha_match:
        errors.append("ENVIRONMENT.md must contain the exact 40-character Git SHA")
    env_sha = sha_match.group(1) if sha_match else None
    for required in ("Working tree clean: `True`", "Build configuration: `Release`", "Client location: `same host / loopback`"):
        if required not in env_text:
            errors.append(f"ENVIRONMENT.md missing required metadata: {required}")

    observed_revisions: set[str] = set()
    for dirname, (mode, concurrency) in EXPECTED.items():
        scenario_dir = bundle / dirname
        summary_path = scenario_dir / "summary.json"
        if not summary_path.is_file():
            errors.append(f"missing {dirname}/summary.json")
            continue
        summary = load_json(summary_path)
        if summary.get("schema_version") != 2:
            errors.append(f"{dirname}: summary schema_version must be 2")
        scenario = summary.get("scenario") or {}
        if scenario.get("client_mode") != mode or scenario.get("concurrency") != concurrency:
            errors.append(f"{dirname}: scenario mode/concurrency mismatch")
        if scenario.get("runs_per_runtime", 0) < 5:
            errors.append(f"{dirname}: need at least 5 runs per runtime")
        runtime_summaries = summary.get("runtime_summaries") or {}
        if set(runtime_summaries) != {"threadpool", "epoll"}:
            errors.append(f"{dirname}: runtime summaries must contain exactly threadpool and epoll")
        for runtime in ("threadpool", "epoll"):
            runtime_summary = runtime_summaries.get(runtime) or {}
            if runtime_summary.get("runs", 0) < 5:
                errors.append(f"{dirname}/{runtime}: fewer than 5 runs")
            revisions = runtime_summary.get("git_revisions") or []
            if len(revisions) != 1 or revisions[0] in ("unknown", ""):
                errors.append(f"{dirname}/{runtime}: evidence must identify one Git revision")
            else:
                observed_revisions.add(str(revisions[0]))
            configs = runtime_summary.get("build_configurations") or []
            if configs != ["Release"]:
                errors.append(f"{dirname}/{runtime}: build configuration must be Release")

        raw_runs = summary.get("raw_runs") or []
        if len(raw_runs) < 10:
            errors.append(f"{dirname}: expected at least 10 raw runs")
        counts = {"threadpool": 0, "epoll": 0}
        for run in raw_runs:
            runtime = run.get("runtime")
            if runtime in counts:
                counts[runtime] += 1
            for field in ("client_json", "server_json", "client_log", "server_log"):
                filename = run.get(field)
                if not filename or not (scenario_dir / filename).is_file():
                    errors.append(f"{dirname}: missing raw file referenced by {field}: {filename}")
        for runtime, count in counts.items():
            if count < 5:
                errors.append(f"{dirname}/{runtime}: only {count} raw runs")

    if len(observed_revisions) > 1:
        errors.append(f"bundle mixes Git revisions: {sorted(observed_revisions)}")
    if env_sha and observed_revisions and observed_revisions != {env_sha}:
        errors.append("ENVIRONMENT.md Git SHA does not match server evidence")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=pathlib.Path)
    args = parser.parse_args()
    try:
        errors = validate(args.bundle)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"M6B.2 bundle validation failed: {exc}")
        return 2
    if errors:
        print("M6B.2 bundle BLOCKED:")
        for error in errors:
            print(f"- {error}")
        return 1
    print("M6B.2 bundle structure is complete and internally consistent.")
    print("This validates evidence structure only; conclusions remain machine/workload-specific.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
