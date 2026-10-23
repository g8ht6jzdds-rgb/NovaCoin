"""Bounded MAINNET refusal checks; never starts a permitted node/network."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    daemon = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="nova-mainnet-lock-") as directory:
        root = Path(directory)
        env = os.environ.copy()
        env["MAINNET_LOCKED"] = "false"  # Must have no effect.
        env["NOVACOIN_RPC_PASSWORD"] = "synthetic-not-a-real-secret"
        cases = [
            ["--mainnet"],
            ["--mainnet", "--datadir", str(root / "never-created")],
            ["--regtest", "--mainnet"],
            ["--mainnet", "--bootstrap", str(root / "missing.conf")],
        ]
        for arguments in cases:
            result = subprocess.run([daemon, *arguments], cwd=root, env=env,
                                    capture_output=True, text=True, timeout=5, check=False)
            if result.returncode != 1 or "MAINNET_LOCKED" not in result.stderr:
                raise AssertionError((arguments, result.returncode, result.stderr))
            if result.stdout or list(root.iterdir()):
                raise AssertionError("locked startup produced output/state")
    print("MAINNET lock: four refusal/side-effect controls passed")


if __name__ == "__main__":
    main()
