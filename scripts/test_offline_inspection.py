"""Disposable sealed Linux fixtures only. Never reads operator chain data."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

INSPECTOR, FIXTURE = sys.argv[1:3]
del sys.argv[1:]


class InspectionBoundary(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="nova-offline-fixture-")
        self.root = Path(self.temp.name)
        self.source = self.root / "source"
        subprocess.run([FIXTURE, str(self.source), "testnet"], check=True, timeout=10)
        self.original = {p.name: p.read_bytes() for p in self.source.iterdir()}
        self.descriptors = []

    def tearDown(self):
        self.assertEqual(self.original, {p.name: p.read_bytes() for p in self.source.iterdir()})
        for fd in self.descriptors:
            os.close(fd)
        self.temp.cleanup()

    def sealed(self, data, seal=True):
        fd = os.memfd_create("nova-synthetic-snapshot", os.MFD_ALLOW_SEALING)
        self.descriptors.append(fd)
        os.write(fd, data)
        if seal:
            fcntl.fcntl(fd, fcntl.F_ADD_SEALS, fcntl.F_SEAL_SEAL | fcntl.F_SEAL_WRITE |
                        fcntl.F_SEAL_GROW | fcntl.F_SEAL_SHRINK)
        return fd

    def run_snapshot(self, name="out", journal=None, identity=None, mutable=False):
        identity = self.original["network.identity"] if identity is None else identity
        journal = self.original["blocks.dat"] if journal is None else journal
        metadata = self.original["snapshot.meta"].replace(
            hashlib.sha256(self.original["blocks.dat"]).hexdigest().encode(),
            hashlib.sha256(journal).hexdigest().encode())
        fds = [self.sealed(identity, not mutable), self.sealed(journal), self.sealed(metadata)]
        output = self.root / name
        output.mkdir(exist_ok=True)
        result = subprocess.run([INSPECTOR, "--sealed-fds", *map(str, fds), str(output)],
                                pass_fds=fds, capture_output=True, timeout=20)
        return result, output

    def test_immutable_seals_reproducibility_and_creator(self):
        first, a = self.run_snapshot("a")
        second, b = self.run_snapshot("b")
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual((a / "report.json").read_bytes(), (b / "report.json").read_bytes())
        report = json.loads((a / "report.json").read_bytes())
        self.assertEqual(report["utxo_value"], "1000000000000000")
        self.assertEqual(report["creator_status"], "unspent")
        with self.assertRaises(OSError):
            os.pwrite(self.descriptors[0], b"x", 0)
        with self.assertRaises(OSError):
            os.ftruncate(self.descriptors[1], 1)

    def test_mutable_and_corrupt_inputs_never_complete(self):
        for name, options in [("mutable", {"mutable": True}),
                              ("marker", {"identity": b"x" * 45}),
                              ("torn", {"journal": b"\x01"}),
                              ("unknown", {"journal": b"\xff"})]:
            result, output = self.run_snapshot(name, **options)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((output / "report.json").exists())

    def test_existing_output_preserved_and_writable_source_refused(self):
        result, output = self.run_snapshot()
        self.assertEqual(result.returncode, 0, result.stderr)
        original = (output / "report.json").read_bytes()
        again, _ = self.run_snapshot()
        self.assertNotEqual(again.returncode, 0)
        self.assertEqual(original, (output / "report.json").read_bytes())
        self.assertEqual([p.name for p in output.iterdir()], ["report.json"])
        self.assertEqual((output / "report.json").stat().st_mode & 0o777, 0o600)
        other = self.root / "other"
        other.mkdir()
        direct = subprocess.run([INSPECTOR, "--snapshot", str(self.source), str(other)],
                                capture_output=True, timeout=20)
        self.assertNotEqual(direct.returncode, 0)
        self.assertEqual(list(other.iterdir()), [])
        alias = self.root / "alias"
        alias.symlink_to(self.source, target_is_directory=True)
        linked = subprocess.run([INSPECTOR, "--snapshot", str(alias), str(other)],
                                capture_output=True, timeout=20)
        self.assertNotEqual(linked.returncode, 0)

    def test_readonly_mount_namespace(self):
        probe = subprocess.run(["unshare", "--user", "--map-root-user", "--mount", "true"],
                               capture_output=True, timeout=10)
        if probe.returncode:
            self.skipTest("user/mount namespaces unavailable; sealed boundary still tested")
        mountpoint = self.root / "mount"
        output = self.root / "readonly-output"
        mountpoint.mkdir()
        output.mkdir()
        code = """
import subprocess,sys
source,target,output,tool=sys.argv[1:]
subprocess.run(['mount','--make-rprivate','/'],check=True)
subprocess.run(['mount','--bind',source,target],check=True)
subprocess.run(['mount','-o','remount,bind,ro',target],check=True)
raise SystemExit(subprocess.run([tool,'--snapshot',target,output]).returncode)
"""
        result = subprocess.run(["unshare", "--user", "--map-root-user", "--mount", "--fork",
                                 sys.executable, "-c", code, str(self.source), str(mountpoint),
                                 str(output), INSPECTOR], capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((output / "report.json").exists())

    def test_sparse_oversize_is_refused_before_read_allocation(self):
        identity = self.sealed(self.original["network.identity"])
        journal = os.memfd_create("nova-oversize-fixture", os.MFD_ALLOW_SEALING)
        self.descriptors.append(journal)
        os.ftruncate(journal, 64 * 1024 * 1024 + 1)
        fcntl.fcntl(journal, fcntl.F_ADD_SEALS, fcntl.F_SEAL_SEAL | fcntl.F_SEAL_WRITE |
                    fcntl.F_SEAL_GROW | fcntl.F_SEAL_SHRINK)
        metadata = self.sealed(self.original["snapshot.meta"])
        output = self.root / "oversize-output"
        output.mkdir()
        fds = [identity, journal, metadata]
        result = subprocess.run([INSPECTOR, "--sealed-fds", *map(str, fds), str(output)],
                                pass_fds=fds, capture_output=True, timeout=20)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"input_not_frozen_regular_bounded", result.stderr)
        self.assertEqual(list(output.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
