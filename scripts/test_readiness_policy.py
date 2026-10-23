"""Synthetic fail-closed gate-state tests, no node execution."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from check_mainnet_readiness import check_references, parse_checklist


def table(status="NOT TESTED"):
    return "\n".join(f"| {number} | requirement | {status} |" for number in range(1, 15))


class ReadinessPolicyTests(unittest.TestCase):
    def test_complete_inventory(self):
        self.assertEqual(len(parse_checklist(table())), 14)
        self.assertTrue(all(row["status"] == "PASS" for row in parse_checklist(table("PASS"))))

    def test_malformed_inventory(self):
        for bad in [table("APPROVED"), table() + "\n| 1 | duplicate | PASS |",
                    table().replace("| 14 |", "| 15 |"), table().splitlines()[0],
                    table().replace("| 1 | requirement |", "| 1 | |")]:
            with self.subTest(inventory=bad), self.assertRaises(ValueError):
                parse_checklist(bad)

    def check_matrix(self, implementation="crypto/keys.cpp", reference="Sample.Case"):
        with tempfile.TemporaryDirectory(prefix="nova-readiness-policy-") as directory:
            root = Path(directory)
            for child in ["tests", "src/crypto", "docs"]:
                (root / child).mkdir(parents=True)
            (root / "tests/sample.cpp").write_text("// synthetic café\nTEST(Sample, Case) {}\n",
                                                  encoding="utf-8")
            (root / "src/crypto/keys.cpp").write_text("// synthetic fixture\n", encoding="utf-8")
            row = f"| C01 rule | {implementation} | `{reference}` | `{reference}` | `{reference}` |"
            if implementation.startswith("NONE:"):
                row = f"| C01 rule | {implementation} | NONE | NONE | NONE |"
            (root / "docs/consensus-coverage.md").write_text(row, encoding="utf-8")
            return check_references(root)

    def test_existing_implementation_and_test(self):
        self.assertEqual(self.check_matrix(), 1)

    def test_missing_implementation(self):
        with self.assertRaisesRegex(ValueError, "implementation reference"):
            self.check_matrix(implementation="crypto/crypto.cpp")

    def test_unsafe_implementation(self):
        for implementation in ["../tests/sample.cpp", "/tests/sample.cpp", "crypto"]:
            with self.subTest(path=implementation), self.assertRaises(ValueError):
                self.check_matrix(implementation=implementation)

    def test_missing_test(self):
        with self.assertRaisesRegex(ValueError, "coverage reference"):
            self.check_matrix(reference="Sample.Missing")

    def test_unimplemented_rule_remains_explicitly_pending(self):
        self.assertEqual(self.check_matrix(implementation="NONE: unapproved"), 1)

    def test_activation_refusal(self):
        script = Path(__file__).with_name("check_mainnet_readiness.py")
        result = subprocess.run([sys.executable, "-B", str(script), "--activation-check"],
                                capture_output=True, text=True, timeout=10, check=False)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("MAINNET_LOCKED", result.stderr)


if __name__ == "__main__":
    unittest.main()
