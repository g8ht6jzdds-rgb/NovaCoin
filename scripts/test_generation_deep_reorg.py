"""Serial, disposable deep-reorg crash tests with complete public-state oracles."""

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

from test_generation_crash import recovery_fixture, require


def main():
    if len(sys.argv) != 4 or sys.argv[3] not in ("1", "10", "100"):
        raise SystemExit("usage: test_generation_deep_reorg.py WORKER INSPECTOR 1|10|100")
    worker = Path(sys.argv[1]).resolve(strict=True)
    inspector = Path(sys.argv[2]).resolve(strict=True)
    depth = int(sys.argv[3])
    prefix = "reorg-" if depth == 1 else f"reorg{depth}-"
    checkpoints = (
        "before-journal", "partial-journal", "journal-close", "journal",
        "before-wallet", "partial-wallet", "wallet-close", "wallet",
        "before-mempool", "partial-mempool", "mempool-close", "mempool",
        "before-descriptor", "partial-descriptor", "descriptor-close", "descriptor",
        "partial-root", "root-close", "before-root", "after-root", "barrier",
    )
    commands = []
    trace = None

    def execute(executable, args, expected=0, timeout=60):
        command = [str(executable), *map(str, args)]
        stem = trace / f"command-{len(commands):04d}"
        stem.with_suffix(".json").write_text(json.dumps({"args": command, "timeout": timeout}))
        try:
            completed = subprocess.run(command, capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired as error:
            stem.with_suffix(".stdout").write_bytes(error.stdout or b"")
            stem.with_suffix(".stderr").write_bytes(error.stderr or b"")
            stem.with_suffix(".timeout").write_text(str(error))
            raise
        stem.with_suffix(".stdout").write_bytes(completed.stdout)
        stem.with_suffix(".stderr").write_bytes(completed.stderr)
        commands.append({"args": [str(executable), *map(str, args)],
                         "exit": completed.returncode,
                         "stdout_sha256": hashlib.sha256(completed.stdout).hexdigest(),
                         "stderr": completed.stderr.decode("utf-8", errors="replace")})
        if completed.returncode != expected:
            raise AssertionError(json.dumps(commands[-1]))
        return completed.stdout

    def snapshot(fixture):
        data = execute(inspector, [fixture, "store", "--full-state"])
        lines = data.decode("ascii").splitlines()
        require(lines[0] == "state-v1")
        active = [line.removeprefix("active=").split(":") for line in lines
                  if line.startswith("active=")]
        require(bool(active))
        cumulative = 0
        # Protocol section 19: exact Python integer oracle, independent of C++ Add.
        for height, row in enumerate(active):
            require(len(row) == 5 and int(row[0]) == height)
            target = int(row[2], 16)
            require(target > 0)
            work = (1 << 256) // (target + 1)
            require(int(row[3], 16) == work)
            cumulative += work
            require(int(row[4], 16) == cumulative)
        state = dict(field.split("=", 1) for field in lines[1].split(";"))
        require(int(state["height"]) == len(active) - 1)
        require(state["tip"] == active[-1][1] and int(state["work"], 16) == cumulative)
        require(lines[-1].startswith("count=") and ";value=" in lines[-1])
        return data, active

    def clone(base, destination):
        require(shutil.disk_usage(destination).free >= 512 * 1024 * 1024)
        (destination / "OWNED-TEST-FIXTURE").write_bytes(b"NOVA-GENERATION-CRASH-FIXTURE\n")
        shutil.copytree(base / "store", destination / "store")
        shutil.copyfile(base / "winning.block", destination / "winning.block")
        shutil.copyfile(base / "payment.id", destination / "payment.id")

    results = []
    with recovery_fixture() as temporary:
        base = Path(temporary).resolve(strict=True)
        trace = base / "evidence"
        trace.mkdir()
        (base / "OWNED-TEST-FIXTURE").write_bytes(b"NOVA-GENERATION-CRASH-FIXTURE\n")
        (base / "store").mkdir()
        require(shutil.disk_usage(base).free >= 512 * 1024 * 1024)
        # The depth-100 fixture commits more than 200 blocks under sanitizers;
        # preserve a bounded deadline without truncating a still-progressing setup.
        init_timeout = 600 if depth == 100 else 180
        winner = execute(worker, [prefix + "init", base], timeout=init_timeout).decode("ascii").strip()
        require(len(winner) == 64)
        before, old_chain = snapshot(base)
        require(len(old_chain) == depth + 2)
        require(execute(worker, [prefix + "payment", base]).strip() == b"absent")
        original = {str(f.relative_to(base)): hashlib.sha256(f.read_bytes()).hexdigest()
                    for f in base.rglob("*") if f.is_file() and trace not in f.parents}
        with recovery_fixture() as temporary_reference:
            reference = Path(temporary_reference).resolve(strict=True)
            clone(base, reference)
            execute(worker, [prefix + "normal", reference])
            expected, new_chain = snapshot(reference)
            require(expected != before and new_chain[-1][1] == winner)
            common = 0
            while common < min(len(old_chain), len(new_chain)) and \
                    old_chain[common][1] == new_chain[common][1]:
                common += 1
            require(common == 2 and len(old_chain) - common == depth)
            require(len(new_chain) - common == depth + 1)
            require(execute(worker, [prefix + "payment", reference]).strip() == b"present")
            for _ in range(2):
                require(snapshot(reference)[0] == expected)
        with recovery_fixture() as temporary_rejection:
            rejected = Path(temporary_rejection).resolve(strict=True)
            clone(base, rejected)
            root_before = (rejected / "store/generation.current").read_bytes()
            execute(worker, [prefix + "reject", rejected])
            for _ in range(2):
                require(snapshot(rejected)[0] == before)
                require((rejected / "store/generation.current").read_bytes() == root_before)
        for checkpoint in checkpoints:
            with recovery_fixture() as temporary_case:
                fixture = Path(temporary_case).resolve(strict=True)
                clone(base, fixture)
                execute(worker, [prefix + checkpoint, fixture], expected=42)
                committed = checkpoint in ("after-root", "barrier")
                observed = None
                for _ in range(2):
                    observed, _ = snapshot(fixture)
                    require(observed == (expected if committed else before))
                    require(execute(worker, [prefix + "payment", fixture]).strip() ==
                            (b"present" if committed else b"absent"))
                results.append({"checkpoint": checkpoint, "child_exit": 42,
                                "restart_observations": 2,
                                "authority": "new" if committed else "old",
                                "public_state_sha256": hashlib.sha256(observed).hexdigest()})
        require(original == {str(f.relative_to(base)): hashlib.sha256(f.read_bytes()).hexdigest()
                             for f in base.rglob("*") if f.is_file() and trace not in f.parents})
        print(json.dumps({"depth": depth, "result": "PASS", "cases": results,
                          "before_public_state": before.decode("ascii"),
                          "new_public_state": expected.decode("ascii"),
                          "commands": commands,
                          "scope": "single-process coordinator; process exits, not power loss"}))


if __name__ == "__main__":
    main()
