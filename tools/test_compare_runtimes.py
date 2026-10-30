import subprocess
import unittest

from compare_runtimes import format_timeout_output


class ClientTimeoutOutputTests(unittest.TestCase):
    def test_preserves_partial_bytes_and_marks_timeout(self):
        exc = subprocess.TimeoutExpired(
            cmd=["stress_http.py"], timeout=2.5, output=b"started\\npartial \\xff"
        )

        self.assertEqual(
            format_timeout_output(exc, 2.5),
            "started\\npartial \\ufffd\\n[ERROR] benchmark client timed out after 2.5s\\n",
        )

    def test_preserves_text_without_adding_duplicate_newline(self):
        exc = subprocess.TimeoutExpired(
            cmd=["stress_http.py"], timeout=1.0, output="partial output\\n"
        )

        self.assertEqual(
            format_timeout_output(exc, 1.0),
            "partial output\\n[ERROR] benchmark client timed out after 1.0s\\n",
        )

    def test_marks_timeout_when_no_partial_output_exists(self):
        exc = subprocess.TimeoutExpired(cmd=["stress_http.py"], timeout=3.0)

        self.assertEqual(
            format_timeout_output(exc, 3.0),
            "[ERROR] benchmark client timed out after 3.0s\\n",
        )


if __name__ == "__main__":
    unittest.main()
