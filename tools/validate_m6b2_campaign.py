#!/usr/bin/env python3
"""Validate completeness and revision consistency for an M6B.2 benchmark campaign."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


def load_json(path: Path) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError(f"expected JSON object: {path}")
    return data


def validate(root: Path) -> dict[str, Any]:
    campaign_path = root / "campaign.json"
    environment_path = root / "ENVIRONMENT.md"
    if not campaign_path.exists():
        raise ValueError("missing campaign.json")
    if not environment_path.exists():
        raise ValueError("missing ENVIRONMENT.md")

    campaign = load_json(campaign_path)
    expected_sha = str(campaign.get("commit_sha", ""))
    expected_runs = int(campaign.get("runs_per_runtime", 0))
    scenarios = campaign.get("scenarios")
    if not expected_sha or expected_sha == "unknown":
        raise ValueError("campaign commit_sha must be identified")
    if expected_runs < 5:
        raise ValueError("publishable M6B.2 campaign requires at least five runs per runtime")
    if not isinstance(scenarios, list) or not scenarios:
        raise ValueError("campaign scenarios must be a non-empty list")

    errors: list[str] = []
    scenario_reports: list[dict[str, Any]] = []
    modes_seen: set[str] = set()

    for scenario_name in scenarios:
        scenario_dir = root / str(scenario_name)
        summary_path = scenario_dir / "summary.json"
        paired_path = scenario_dir / "paired-analysis.json"
        if not summary_path.exists():
            errors.append(f"{scenario_name}: missing summary.json")
            continue
        if not paired_path.exists():
            errors.append(f"{scenario_name}: missing paired-analysis.json")
            continue

        summary = load_json(summary_path)
        paired = load_json(paired_path)
        raw_runs = summary.get("raw_runs")
        runtime_summaries = summary.get("runtime_summaries")
        if not isinstance(raw_runs, list):
            errors.append(f"{scenario_name}: summary missing raw_runs")
            continue
        if len(raw_runs) != expected_runs * 2:
            errors.append(
                f"{scenario_name}: expected {expected_runs * 2} raw runs, got {len(raw_runs)}"
            )
        if not isinstance(runtime_summaries, dict):
            errors.append(f"{scenario_name}: missing runtime_summaries")
            continue
        if set(runtime_summaries) != {"threadpool", "epoll"}:
            errors.append(f"{scenario_name}: runtime set must be exactly threadpool + epoll")

        revisions: set[str] = set()
        for runtime in ("threadpool", "epoll"):
            runtime_data = runtime_summaries.get(runtime)
            if not isinstance(runtime_data, dict):
                continue
            revs = runtime_data.get("git_revisions")
            if not isinstance(revs, list) or len(revs) != 1:
                errors.append(f"{scenario_name}: {runtime} must report exactly one Git revision")
                continue
            revisions.add(str(revs[0]))
            median = runtime_data.get("median")
            if not isinstance(median, dict):
                errors.append(f"{scenario_name}: {runtime} median metrics missing")
                continue
            failure_rate = median.get("failure_rate")
            if failure_rate is None:
                errors.append(f"{scenario_name}: {runtime} failure rate missing")

        if revisions and revisions != {expected_sha}:
            errors.append(
                f"{scenario_name}: benchmark revision(s) {sorted(revisions)} do not match campaign {expected_sha}"
            )

        pair_count = int(paired.get("pairs", 0))
        if pair_count != expected_runs:
            errors.append(f"{scenario_name}: expected {expected_runs} paired runs, got {pair_count}")

        scenario = summary.get("scenario")
        if isinstance(scenario, dict):
            modes_seen.add(str(scenario.get("client_mode", "unknown")))

        scenario_reports.append(
            {
                "scenario": str(scenario_name),
                "raw_runs": len(raw_runs),
                "pairs": pair_count,
                "git_revisions": sorted(revisions),
            }
        )

    if not {"keepalive", "connect"}.issubset(modes_seen):
        errors.append("campaign must include both keepalive and connect client modes")

    return {
        "schema_version": 1,
        "evidence_type": "m6b2_campaign_validation",
        "claim_scope": "documented local Linux/WSL2 host and recorded scenarios only",
        "commit_sha": expected_sha,
        "runs_per_runtime": expected_runs,
        "scenario_count": len(scenarios),
        "scenarios_checked": scenario_reports,
        "client_modes_seen": sorted(modes_seen),
        "complete": not errors,
        "errors": errors,
        "limitations": [
            "A complete campaign does not prove internet-facing capacity.",
            "The first-party Python client can become a bottleneck at high request rates.",
            "WSL2 evidence must remain labelled WSL2 and not be generalized to native Linux.",
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("campaign_dir", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    try:
        report = validate(args.campaign_dir)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        report = {"complete": False, "errors": [str(exc)]}

    text = json.dumps(report, indent=2, sort_keys=True)
    print(text)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text + "\n", encoding="utf-8")
    return 0 if report.get("complete") else 1


if __name__ == "__main__":
    raise SystemExit(main())
