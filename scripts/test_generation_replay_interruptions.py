"""Interrupt recovery of a committed reorg with a nonempty pending wallet."""

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

from test_generation_crash import recovery_fixture, require


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_generation_replay_interruptions.py WORKER INSPECTOR")
    worker, inspector = (Path(arg).resolve(strict=True) for arg in sys.argv[1:])
    commands = []
    cases = []
    with recovery_fixture() as temporary:
        base = Path(temporary).resolve(strict=True)
        trace = base / "evidence"
        trace.mkdir()
        (base / "OWNED-TEST-FIXTURE").write_bytes(b"NOVA-GENERATION-CRASH-FIXTURE\n")
        (base / "store").mkdir()
        require(shutil.disk_usage(base).free >= 512 * 1024 * 1024)

        def run(executable, args, expected=0, timeout=60):
            command = [str(executable), *map(str, args)]
            stem = trace / f"command-{len(commands):04d}"
            started = time.monotonic()
            stem.with_suffix(".json").write_text(json.dumps({"args": command, "timeout": timeout}))
            try:
                result = subprocess.run(command, capture_output=True, timeout=timeout)
            except subprocess.TimeoutExpired as error:
                stem.with_suffix(".stdout").write_bytes(error.stdout or b"")
                stem.with_suffix(".stderr").write_bytes(error.stderr or b"")
                stem.with_suffix(".timeout").write_text(str(error))
                raise
            stem.with_suffix(".stdout").write_bytes(result.stdout)
            stem.with_suffix(".stderr").write_bytes(result.stderr)
            commands.append({"args": command, "exit": result.returncode,
                             "elapsed_seconds": time.monotonic() - started,
                             "stdout_sha256": hashlib.sha256(result.stdout).hexdigest(),
                             "stderr": result.stderr.decode("utf-8", errors="replace")})
            if result.returncode != expected:
                raise AssertionError(json.dumps(commands[-1]))
            return result.stdout

        def files(root):
            return {str(f.relative_to(root)): hashlib.sha256(f.read_bytes()).hexdigest()
                    for f in root.rglob("*") if f.is_file()}

        run(worker, ["reorg10-init", base], timeout=180)
        run(worker, ["reorg10-normal", base])
        require(run(worker, ["reorg10-payment", base]).strip() == b"present")
        before = run(inspector, [base, "store", "--full-state"])
        lines = before.splitlines()
        require(lines[0] == b"state-v1" and any(line.startswith(b"pending=1:") for line in lines))
        original = files(base / "store")
        modes = ("before", "first", "middle", "last", "before-wallet", "wallet",
                 "before-mempool", "mempool", "admission")
        for mode in modes:
            with recovery_fixture() as temporary_case:
                fixture = Path(temporary_case).resolve(strict=True)
                require(shutil.disk_usage(fixture).free >= 512 * 1024 * 1024)
                (fixture / "OWNED-TEST-FIXTURE").write_bytes(b"NOVA-GENERATION-CRASH-FIXTURE\n")
                shutil.copytree(base / "store", fixture / "store")
                checkpoint = run(worker, ["reorg10-replay-" + mode, fixture], expected=42)
                fields = dict(field.split("=", 1) for field in checkpoint.decode("ascii").strip().split(";"))
                require(fields["recovery-checkpoint"] == "replay-" + mode)
                total, completed = int(fields["total"]), int(fields["completed"])
                require(total > 2)
                expected = {"before": 0, "first": 1, "middle": (total + 1) // 2}.get(mode, total)
                require(completed == expected)
                require(files(fixture / "store") == original)
                for _ in range(2):
                    require(run(inspector, [fixture, "store", "--full-state"]) == before)
                    require(files(fixture / "store") == original)
                cases.append({"mode": mode, "completed": completed, "total": total,
                              "child_exit": 42, "restart_observations": 2,
                              "state_sha256": hashlib.sha256(before).hexdigest()})
        require(files(base / "store") == original)
        print(json.dumps({"result": "PASS", "cases": cases, "commands": commands,
                          "public_state": before.decode("ascii"),
                          "scope": "recovery process exits; not power loss"}))


if __name__ == "__main__":
    main()
