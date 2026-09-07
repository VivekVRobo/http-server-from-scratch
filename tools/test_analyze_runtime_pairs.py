import json
import tempfile
import unittest
from pathlib import Path

from analyze_runtime_pairs import analyze


class PairedAnalysisTests(unittest.TestCase):
    def write_client(self, directory: Path, name: str, rps: float, p95: float, p99: float) -> None:
        payload = {
            "results": {
                "successful_requests_per_second": rps,
                "failure_rate": 0.0,
                "success_latency": {"p95_ms": p95, "p99_ms": p99},
            }
        }
        (directory / name).write_text(json.dumps(payload), encoding="utf-8")

    def test_paired_percent_deltas(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            raw_runs = []
            for index, (tp_rps, ep_rps, tp_p95, ep_p95) in enumerate(
                [(100.0, 120.0, 10.0, 8.0), (110.0, 121.0, 11.0, 9.9)],
                start=1,
            ):
                tp = f"threadpool-run-{index:02d}-client.json"
                ep = f"epoll-run-{index:02d}-client.json"
                self.write_client(root, tp, tp_rps, tp_p95, tp_p95 * 1.2)
                self.write_client(root, ep, ep_rps, ep_p95, ep_p95 * 1.2)
                raw_runs.extend(
                    [
                        {"runtime": "threadpool", "run_index": index, "client_json": tp},
                        {"runtime": "epoll", "run_index": index, "client_json": ep},
                    ]
                )
            (root / "summary.json").write_text(json.dumps({"raw_runs": raw_runs}), encoding="utf-8")

            report = analyze(root)
            self.assertEqual(report["pairs"], 2)
            self.assertAlmostEqual(
                report["median_epoll_vs_threadpool_percent"]["successful_requests_per_second"],
                15.0,
            )
            self.assertLess(report["median_epoll_vs_threadpool_percent"]["success_p95_ms"], 0.0)
            self.assertEqual(report["direction_consistency"]["throughput_epoll_higher_pairs"], 2)

    def test_incomplete_pair_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.write_client(root, "threadpool-run-01-client.json", 100.0, 10.0, 12.0)
            summary = {
                "raw_runs": [
                    {"runtime": "threadpool", "run_index": 1, "client_json": "threadpool-run-01-client.json"}
                ]
            }
            (root / "summary.json").write_text(json.dumps(summary), encoding="utf-8")
            with self.assertRaises(ValueError):
                analyze(root)


if __name__ == "__main__":
    unittest.main()
