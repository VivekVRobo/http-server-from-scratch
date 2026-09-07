import json
import tempfile
import unittest
from pathlib import Path

from validate_m6b2_campaign import validate


SHA = "a" * 40


def write_json(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data), encoding="utf-8")


def write_scenario(root: Path, name: str, mode: str, revision: str = SHA) -> None:
    scenario = root / name
    raw_runs = []
    for run_index in range(1, 6):
        raw_runs.extend(
            [
                {"runtime": "threadpool", "run_index": run_index},
                {"runtime": "epoll", "run_index": run_index},
            ]
        )
    write_json(
        scenario / "summary.json",
        {
            "scenario": {"client_mode": mode},
            "raw_runs": raw_runs,
            "runtime_summaries": {
                "threadpool": {
                    "git_revisions": [revision],
                    "median": {"failure_rate": 0.0},
                },
                "epoll": {
                    "git_revisions": [revision],
                    "median": {"failure_rate": 0.0},
                },
            },
        },
    )
    write_json(scenario / "paired-analysis.json", {"pairs": 5})


class CampaignValidatorTests(unittest.TestCase):
    def make_campaign(self, root: Path) -> None:
        (root / "ENVIRONMENT.md").write_text("# environment\n", encoding="utf-8")
        write_json(
            root / "campaign.json",
            {
                "commit_sha": SHA,
                "runs_per_runtime": 5,
                "scenarios": ["keepalive-c8", "connect-c64"],
            },
        )
        write_scenario(root, "keepalive-c8", "keepalive")
        write_scenario(root, "connect-c64", "connect")

    def test_complete_campaign_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.make_campaign(root)
            report = validate(root)
            self.assertTrue(report["complete"])
            self.assertEqual(report["client_modes_seen"], ["connect", "keepalive"])

    def test_mixed_revision_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.make_campaign(root)
            write_scenario(root, "connect-c64", "connect", revision="b" * 40)
            report = validate(root)
            self.assertFalse(report["complete"])
            self.assertTrue(any("do not match campaign" in error for error in report["errors"]))


if __name__ == "__main__":
    unittest.main()
