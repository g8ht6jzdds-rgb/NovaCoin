"""Parent-owned abrupt-exit/restart tests; no services, secrets or host crashes."""

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def require(condition):
    if not condition:
        raise AssertionError("generation crash/restart invariant failed")


def run(worker, mode, fixture, expected_code):
    result = subprocess.run(
        [str(worker), mode, str(fixture)], capture_output=True, text=True, timeout=30
    )
    if result.returncode != expected_code:
        raise AssertionError(
            f"{mode}: return code {result.returncode}, expected {expected_code}; "
            f"stdout={result.stdout!r}; stderr={result.stderr!r}"
        )
    return result.stdout.strip()


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_generation_crash.py TEST_WORKER")
    worker = Path(sys.argv[1]).resolve(strict=True)
    results = []
    for checkpoint in ("journal", "wallet", "descriptor", "before-root", "after-root", "barrier"):
        with tempfile.TemporaryDirectory(prefix="novacoin-generation-crash-") as temporary:
            fixture = Path(temporary).resolve(strict=True)
            fixture.relative_to(Path(tempfile.gettempdir()).resolve())
            (fixture / "OWNED-TEST-FIXTURE").write_bytes(
                b"NOVA-GENERATION-CRASH-FIXTURE\n"
            )
            store = fixture / "store"
            store.mkdir()
            run(worker, "unknown-mode", fixture, 10)
            require(not list(store.iterdir()))
            (fixture / "OWNED-TEST-FIXTURE").write_bytes(b"x" * 512)
            run(worker, "normal", fixture, 11)
            require(not list(store.iterdir()))
            (fixture / "OWNED-TEST-FIXTURE").write_bytes(
                b"NOVA-GENERATION-CRASH-FIXTURE\n"
            )
            old_journal = b"old controlled journal image"
            old_wallet = b"old non-secret test wallet image"
            (fixture / "input.journal").write_bytes(old_journal)
            (fixture / "input.wallet").write_bytes(old_wallet)
            run(worker, "normal", fixture, 0)
            require(run(worker, "inspect", fixture, 0) == "1")
            old_root = (store / "generation.current").read_bytes()
            (fixture / "input.journal").write_bytes(b"new controlled journal image")
            (fixture / "input.wallet").write_bytes(b"new non-secret test wallet image")
            run(worker, checkpoint, fixture, 42)
            expected = 2 if checkpoint in ("after-root", "barrier") else 1
            for _ in range(2):
                require(run(worker, "inspect", fixture, 0) == str(expected))
            suffix = f"generation-{expected:016x}"
            journal = (store / (suffix + ".journal")).read_bytes()
            wallet = (store / (suffix + ".wallet")).read_bytes()
            if expected == 1:
                require((store / "generation.current").read_bytes() == old_root)
                require(journal == old_journal and wallet == old_wallet)
            else:
                require(journal == (fixture / "input.journal").read_bytes())
                require(wallet == (fixture / "input.wallet").read_bytes())
            require((store / "generation-0000000000000001.journal").read_bytes() == old_journal)
            require((store / "generation-0000000000000001.wallet").read_bytes() == old_wallet)
            results.append(
                {
                    "checkpoint": checkpoint,
                    "child_exit": 42,
                    "restart_sequence": expected,
                    "restart_observations": 2,
                    "journal_sha256": hashlib.sha256(journal).hexdigest(),
                    "wallet_sha256": hashlib.sha256(wallet).hexdigest(),
                }
            )
    print(json.dumps({"scope": "controlled abrupt process exit; not hardware power loss", "results": results}))


if __name__ == "__main__":
    main()
