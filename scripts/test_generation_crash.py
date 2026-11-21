"""Parent-owned abrupt-exit/restart tests; no services, secrets or host crashes."""

import hashlib
import json
from contextlib import contextmanager
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


@contextmanager
def recovery_fixture(prefix="novacoin-generation-crash-"):
    """Delete successful owned fixtures; retain all evidence on failure."""
    require(prefix in ("novacoin-generation-crash-", "novacoin-durable-integration-",
                       "novacoin-durable-reorg-"))
    directory = Path(tempfile.mkdtemp(prefix=prefix))
    try:
        yield str(directory)
    except BaseException:
        print(f"Retained failed recovery fixture: {directory}", file=sys.stderr, flush=True)
        raise
    else:
        # Resolve before recursive removal, and never follow a replaced fixture.
        require(not directory.is_symlink())
        require(directory.resolve(strict=True).parent == Path(tempfile.gettempdir()).resolve())
        require(directory.name.startswith(prefix))
        shutil.rmtree(directory)


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
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_generation_crash.py TEST_WORKER UTXO_INSPECTOR")
    worker = Path(sys.argv[1]).resolve(strict=True)
    inspector = Path(sys.argv[2]).resolve(strict=True)

    def utxos(fixture, testnet=False, full_state=False):
        result = subprocess.run([str(inspector), str(fixture),
                                 "testnet-store" if testnet else "store"] +
                                (["--full-state"] if full_state else []),
                                capture_output=True, timeout=180)
        if result.returncode != 0:
            raise RuntimeError(f"UTXO inspection failed: {result.returncode} {result.stderr!r}")
        return result.stdout.replace(b"\r\n", b"\n") if full_state else result.stdout
    writer_checkpoints = (
        "before-journal", "partial-journal", "journal-close", "journal",
        "before-wallet", "partial-wallet", "wallet-close", "wallet",
        "before-mempool", "partial-mempool", "mempool-close", "mempool",
        "before-descriptor", "partial-descriptor", "descriptor-close", "descriptor",
        "partial-root", "root-close", "before-root", "after-root", "barrier",
    )
    results = []
    for testnet in (False, True):
        for checkpoint in ("partial", "close", "prepare-barrier", "before-root",
                           "after-root", "barrier", "recovery-barrier"):
            with recovery_fixture() as temporary:
                fixture = Path(temporary).resolve(strict=True)
                (fixture / "OWNED-TEST-FIXTURE").write_bytes(
                    b"NOVA-GENERATION-CRASH-FIXTURE\n")
                store = fixture / "store"
                store.mkdir()
                prefix = "testnet-" if testnet else ""
                run(worker, prefix + "startup-" + checkpoint, fixture, 42)
                replaced = checkpoint in ("after-root", "barrier", "recovery-barrier")
                require((store / "generation.current").exists() == replaced)
                before = {f.name: f.read_bytes() for f in store.iterdir() if f.is_file()}
                for _ in range(2):
                    observed = run(worker, prefix + "inspect", fixture, 0 if replaced else 12)
                    require(observed == ("0" if replaced else ""))
                    require(before == {f.name: f.read_bytes() for f in store.iterdir()
                                       if f.is_file()})
                results.append({"startup": checkpoint, "testnet": testnet,
                                "child_exit": 42, "restart_observations": 2,
                                "result": "empty" if replaced else "refused"})
    checkpoints = (
        "partial-journal", "partial-wallet", "before-journal", "before-wallet",
        "journal", "wallet", "partial-descriptor", "partial-root", "before-descriptor",
        "descriptor", "before-root", "after-root", "barrier"
    )
    cases = [(False, point) for point in checkpoints] + [
        (True, point)
        for point in (
            "partial-journal", "partial-wallet", "before-journal", "before-wallet", "partial-mempool", "before-mempool", "journal", "wallet",
            "mempool", "partial-descriptor", "partial-root", "before-descriptor", "descriptor", "before-root", "after-root", "barrier"
        )
    ]
    for with_mempool, checkpoint in cases:
        with recovery_fixture() as temporary:
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
            old_mempool = b"old controlled mempool image"
            (fixture / "input.journal").write_bytes(old_journal)
            (fixture / "input.wallet").write_bytes(old_wallet)
            if with_mempool:
                (fixture / "input.mempool").write_bytes(old_mempool)
            run(worker, "v2-normal" if with_mempool else "normal", fixture, 0)
            require(run(worker, "inspect", fixture, 0) == "1")
            old_root = (store / "generation.current").read_bytes()
            (fixture / "input.journal").write_bytes(b"new controlled journal image")
            (fixture / "input.wallet").write_bytes(b"new non-secret test wallet image")
            if with_mempool:
                (fixture / "input.mempool").write_bytes(b"new controlled mempool image")
            run(worker, ("v2-" if with_mempool else "") + checkpoint, fixture, 42)
            partial_bytes = None
            partial_path = None
            if checkpoint.startswith("partial-"):
                member = checkpoint[len("partial-"):]
                partial_path = store / ("generation-0000000000000002." + member)
                partial_bytes = partial_path.read_bytes()
                expected_prefix = (
                    (fixture / ("input." + member)).read_bytes()[:1]
                    if member in ("journal", "wallet", "mempool") else b"N"
                )
                require(partial_bytes == expected_prefix)
            expected = 2 if checkpoint in ("after-root", "barrier") else 1
            for _ in range(2):
                require(run(worker, "inspect", fixture, 0) == str(expected))
            suffix = f"generation-{expected:016x}"
            journal = (store / (suffix + ".journal")).read_bytes()
            wallet = (store / (suffix + ".wallet")).read_bytes()
            mempool = (store / (suffix + ".mempool")).read_bytes() if with_mempool else None
            if expected == 1:
                require((store / "generation.current").read_bytes() == old_root)
                require(journal == old_journal and wallet == old_wallet)
                if with_mempool:
                    require(mempool == old_mempool)
            else:
                require(journal == (fixture / "input.journal").read_bytes())
                require(wallet == (fixture / "input.wallet").read_bytes())
                if with_mempool:
                    require(mempool == (fixture / "input.mempool").read_bytes())
            require((store / "generation-0000000000000001.journal").read_bytes() == old_journal)
            require((store / "generation-0000000000000001.wallet").read_bytes() == old_wallet)
            if partial_path is not None:
                require(partial_path.read_bytes() == partial_bytes)
            if with_mempool:
                require(
                    (store / "generation-0000000000000001.mempool").read_bytes() == old_mempool
                )
            results.append(
                {
                    "checkpoint": checkpoint,
                    "format": "journal-wallet-mempool" if with_mempool else "journal-wallet",
                    "child_exit": 42,
                    "restart_sequence": expected,
                    "restart_observations": 2,
                    "partial_candidate_bytes": (
                        len(partial_bytes) if partial_bytes is not None else None
                    ),
                    "journal_sha256": hashlib.sha256(journal).hexdigest(),
                    "wallet_sha256": hashlib.sha256(wallet).hexdigest(),
                    "mempool_sha256": (
                        hashlib.sha256(mempool).hexdigest() if mempool is not None else None
                    ),
                }
            )
    for checkpoint in writer_checkpoints:
        with recovery_fixture() as temporary:
            fixture = Path(temporary).resolve(strict=True)
            fixture.relative_to(Path(tempfile.gettempdir()).resolve())
            (fixture / "OWNED-TEST-FIXTURE").write_bytes(b"NOVA-GENERATION-CRASH-FIXTURE\n")
            (fixture / "store").mkdir()
            run(worker, "session-init", fixture, 0)
            before = run(worker, "session-inspect", fixture, 0)
            require(before.split()[:4] == ["0", "0", "0", "0"])
            run(worker, "session-" + checkpoint, fixture, 42)
            addresses = 1 if checkpoint in ("after-root", "barrier") else 0
            after = run(worker, "session-inspect", fixture, 0)
            after_fields = after.split()
            require(len(after_fields) == 8 and after_fields[:4] == ["0", str(addresses), "0", "0"])
            require(after_fields[4:] == before.split()[4:])
            require(run(worker, "session-inspect", fixture, 0) == after)
            results.append(
                {
                    "checkpoint": checkpoint,
                    "format": "durable-session-address",
                    "child_exit": 42,
                    "restart_observations": 2,
                    "height": 0,
                    "address_count": addresses,
                    "mempool_count": 0,
                }
            )
    for checkpoint in writer_checkpoints:
        with recovery_fixture() as temporary:
            fixture = Path(temporary).resolve(strict=True)
            fixture.relative_to(Path(tempfile.gettempdir()).resolve())
            (fixture / "OWNED-TEST-FIXTURE").write_bytes(b"NOVA-GENERATION-CRASH-FIXTURE\n")
            (fixture / "store").mkdir()
            run(worker, "session-init", fixture, 0)
            run(worker, "session-mine-normal", fixture, 0)
            before = run(worker, "session-inspect", fixture, 0)
            before_fields = before.split()
            require(len(before_fields) == 8 and before_fields[:3] == ["1", "1", "0"])
            require(int(before_fields[3]) > 0)
            run(worker, "session-mine-" + checkpoint, fixture, 42)
            after = run(worker, "session-inspect", fixture, 0)
            require(run(worker, "session-inspect", fixture, 0) == after)
            after_fields = after.split()
            require(len(after_fields) == 8)
            if checkpoint in ("after-root", "barrier"):
                require(after_fields[:3] == ["2", "1", "0"])
                require(int(after_fields[3]) > int(before_fields[3]))
                require(after_fields[4] != before_fields[4])
            else:
                require(after == before)
            results.append(
                {
                    "checkpoint": checkpoint,
                    "format": "durable-session-block",
                    "child_exit": 42,
                    "restart_observations": 2,
                    "before": before,
                    "after": after,
                }
            )
    for checkpoint in writer_checkpoints:
        with recovery_fixture() as temporary, \
             recovery_fixture() as reference:
            fixture = Path(temporary).resolve(strict=True)
            reference_fixture = Path(reference).resolve(strict=True)
            fixture.relative_to(Path(tempfile.gettempdir()).resolve())
            reference_fixture.relative_to(Path(tempfile.gettempdir()).resolve())
            for path in (fixture, reference_fixture):
                (path / "OWNED-TEST-FIXTURE").write_bytes(b"NOVA-GENERATION-CRASH-FIXTURE\n")
            (fixture / "store").mkdir()
            run(worker, "session-init", fixture, 0)
            run(worker, "session-mine-normal", fixture, 0)
            before = run(worker, "session-inspect", fixture, 0)
            before_utxos = utxos(fixture)
            shutil.copytree(fixture / "store", reference_fixture / "store")
            run(worker, "runtime-mine-normal", reference_fixture, 0)
            committed = run(worker, "session-inspect", reference_fixture, 0)
            committed_utxos = utxos(reference_fixture)
            require(committed != before and committed_utxos != before_utxos)
            run(worker, "runtime-mine-" + checkpoint, fixture, 42)
            after = run(worker, "session-inspect", fixture, 0)
            require(run(worker, "session-inspect", fixture, 0) == after)
            after_utxos = utxos(fixture)
            expected_new = checkpoint in ("after-root", "barrier")
            require(after == (committed if expected_new else before))
            require(after_utxos == (committed_utxos if expected_new else before_utxos))
            results.append({"checkpoint": checkpoint, "format": "durable-rpc-block",
                            "child_exit": 42, "restart_observations": 2,
                            "before": before, "after": after,
                            "utxo_sha256": hashlib.sha256(after_utxos).hexdigest()})
    for operation in ("pay", "admit", "mine-pending"):
        for checkpoint in writer_checkpoints:
            with recovery_fixture() as temporary:
                fixture = Path(temporary).resolve(strict=True)
                fixture.relative_to(Path(tempfile.gettempdir()).resolve())
                (fixture / "OWNED-TEST-FIXTURE").write_bytes(b"NOVA-GENERATION-CRASH-FIXTURE\n")
                (fixture / "store").mkdir()
                run(worker, "session-init", fixture, 0)
                run(worker, "session-mine-normal", fixture, 0)
                if operation == "mine-pending":
                    run(worker, "session-pay-normal", fixture, 0)
                before = run(worker, "session-inspect", fixture, 0)
                fields = before.split()
                require(len(fields) == 8 and fields[:2] == ["1", "1"])
                require(fields[2] == ("1" if operation == "mine-pending" else "0"))
                mode = "mine" if operation == "mine-pending" else operation
                before_state = utxos(fixture, full_state=True) if operation == "admit" else None
                run(worker, "session-" + mode + "-" + checkpoint, fixture, 42)
                after = run(worker, "session-inspect", fixture, 0)
                require(run(worker, "session-inspect", fixture, 0) == after)
                if before_state is not None:
                    after_state = utxos(fixture, full_state=True)
                    require(utxos(fixture, full_state=True) == after_state)
                    if checkpoint in ("after-root", "barrier"):
                        before_metadata, before_coins = before_state.split(b"\nutxos\n", 1)
                        after_metadata, after_coins = after_state.split(b"\nutxos\n", 1)
                        require(after_coins == before_coins)
                        for prefix in (b"state-v1", b"height=", b"active=", b"public-key="):
                            expected = [line for line in before_metadata.splitlines()
                                        if line.startswith(prefix)]
                            require(expected and expected == [
                                line for line in after_metadata.splitlines() if line.startswith(prefix)])
                        require(b"\npending=0:" in before_metadata and b"\npending=1:" in after_metadata)
                    else:
                        require(after_state == before_state)
                after_fields = after.split()
                require(len(after_fields) == 8)
                if checkpoint in ("after-root", "barrier"):
                    require(after_fields[:3] == (
                        ["2", "1", "0"] if operation == "mine-pending" else ["1", "1", "1"]
                    ))
                    if operation == "mine-pending":
                        require(after_fields[4] != fields[4] and after_fields[7] == "0")
                    else:
                        require(after_fields[4] == fields[4] and int(after_fields[7]) > 0)
                else:
                    require(after == before)
                results.append(
                    {
                        "checkpoint": checkpoint,
                        "format": "durable-session-" + operation,
                        "child_exit": 42,
                        "restart_observations": 2,
                        "before": before,
                        "after": after,
                    }
                )
    for checkpoint in writer_checkpoints:
        with recovery_fixture() as temporary, \
             recovery_fixture() as reference:
            fixture = Path(temporary).resolve(strict=True)
            reference_fixture = Path(reference).resolve(strict=True)
            fixture.relative_to(Path(tempfile.gettempdir()).resolve())
            reference_fixture.relative_to(Path(tempfile.gettempdir()).resolve())
            (fixture / "OWNED-TEST-FIXTURE").write_bytes(b"NOVA-GENERATION-CRASH-FIXTURE\n")
            (fixture / "store").mkdir()
            winner = run(worker, "reorg-init", fixture, 0)
            require(len(winner) == 64)
            before = run(worker, "session-inspect", fixture, 0)
            fields = before.split()
            require(len(fields) == 8 and fields[:3] == ["2", "1", "0"])
            require(run(worker, "reorg-payment", fixture, 0) == "absent")
            before_utxos = utxos(fixture)
            (reference_fixture / "OWNED-TEST-FIXTURE").write_bytes(b"NOVA-GENERATION-CRASH-FIXTURE\n")
            shutil.copytree(fixture / "store", reference_fixture / "store")
            shutil.copyfile(fixture / "winning.block", reference_fixture / "winning.block")
            run(worker, "reorg-normal", reference_fixture, 0)
            expected_utxos = utxos(reference_fixture)
            require(expected_utxos != before_utxos)
            run(worker, "reorg-" + checkpoint, fixture, 42)
            after = run(worker, "session-inspect", fixture, 0)
            require(run(worker, "session-inspect", fixture, 0) == after)
            after_fields = after.split()
            if checkpoint in ("after-root", "barrier"):
                require(len(after_fields) == 8 and after_fields[:3] == ["3", "1", "1"])
                require(after_fields[4] == winner and int(after_fields[3]) < int(fields[3]))
                require(int(after_fields[7]) > 0)
                require(run(worker, "reorg-payment", fixture, 0) == "present")
            else:
                require(after == before)
                require(run(worker, "reorg-payment", fixture, 0) == "absent")
            observed_utxos = utxos(fixture)
            require(observed_utxos == (expected_utxos if checkpoint in ("after-root", "barrier")
                                       else before_utxos))
            results.append(
                {
                    "checkpoint": checkpoint,
                    "format": "durable-session-reorg",
                    "child_exit": 42,
                    "restart_observations": 2,
                    "before": before,
                    "after": after,
                    "winning_tip": winner,
                    "utxo_sha256": hashlib.sha256(observed_utxos).hexdigest(),
                }
            )
    for checkpoint in writer_checkpoints:
        with recovery_fixture() as temporary, \
             recovery_fixture() as reference:
            fixture = Path(temporary).resolve(strict=True)
            reference_fixture = Path(reference).resolve(strict=True)
            fixture.relative_to(Path(tempfile.gettempdir()).resolve())
            reference_fixture.relative_to(Path(tempfile.gettempdir()).resolve())
            (fixture / "OWNED-TEST-FIXTURE").write_bytes(b"NOVA-GENERATION-CRASH-FIXTURE\n")
            (fixture / "store").mkdir()
            run(worker, "testnet-session-init", fixture, 0)
            before = run(worker, "testnet-session-inspect", fixture, 0)
            require(before.split()[:4] == ["0", "0", "0", "0"])
            before_utxos = utxos(fixture, testnet=True)
            (reference_fixture / "OWNED-TEST-FIXTURE").write_bytes(
                b"NOVA-GENERATION-CRASH-FIXTURE\n")
            shutil.copytree(fixture / "store", reference_fixture / "store")
            run(worker, "testnet-session-block-normal", reference_fixture, 0)
            committed_utxos = utxos(reference_fixture, testnet=True)
            require(committed_utxos != before_utxos)
            run(worker, "testnet-session-block-" + checkpoint, fixture, 42)
            after = run(worker, "testnet-session-inspect", fixture, 0)
            require(run(worker, "testnet-session-inspect", fixture, 0) == after)
            after_fields = after.split()
            require(len(after_fields) == 8)
            if checkpoint in ("after-root", "barrier"):
                require(after_fields[:4] == ["1", "0", "0", "0"])
                require(after_fields[4] != before.split()[4])
            else:
                require(after == before)
            actual_utxos = utxos(fixture, testnet=True)
            require(actual_utxos == (committed_utxos if checkpoint in ("after-root", "barrier")
                                     else before_utxos))
            results.append({"checkpoint": checkpoint, "format": "testnet-session-block",
                            "child_exit": 42, "restart_observations": 2,
                            "before": before, "after": after,
                            "utxo_sha256": hashlib.sha256(actual_utxos).hexdigest()})
    # Legacy migration prepares journal and wallet images without a mempool image.
    for checkpoint in (point for point in writer_checkpoints if "mempool" not in point):
        with recovery_fixture() as temporary:
            fixture = Path(temporary).resolve(strict=True)
            fixture.relative_to(Path(tempfile.gettempdir()).resolve())
            (fixture / "OWNED-TEST-FIXTURE").write_bytes(b"NOVA-GENERATION-CRASH-FIXTURE\n")
            (fixture / "store").mkdir()
            original = run(worker, "migration-init", fixture, 0)
            require(len(original.split()) == 3 and original.split()[0] == "1")
            source = [fixture / "source" / "blocks.dat", fixture / "source" / "wallet.dat"]
            original_hashes = [hashlib.sha256(path.read_bytes()).hexdigest() for path in source]
            selector = fixture / "store/generation.current"
            before_selector = selector.read_bytes() if selector.exists() else None
            run(worker, "migration-" + checkpoint, fixture, 42)
            expected = original if checkpoint in ("after-root", "barrier") else "empty"
            require(run(worker, "migration-inspect", fixture, 0) == expected)
            require(run(worker, "migration-inspect", fixture, 0) == expected)
            require(
                [hashlib.sha256(path.read_bytes()).hexdigest() for path in source]
                == original_hashes
            )
            if expected == "empty":
                require((selector.read_bytes() if selector.exists() else None) == before_selector)
                state_hash = None
            else:
                with recovery_fixture() as reference:
                    reference = Path(reference).resolve(strict=True)
                    shutil.copyfile(fixture / "OWNED-TEST-FIXTURE", reference / "OWNED-TEST-FIXTURE")
                    shutil.copytree(fixture / "source", reference / "source")
                    (reference / "store").mkdir()
                    run(worker, "migration-normal", reference, 0)
                    committed_state = utxos(reference, full_state=True)
                require(b"\npublic-key=" in committed_state and b"\npending=0:" in committed_state)
                for _ in range(2):
                    require(utxos(fixture, full_state=True) == committed_state)
                state_hash = hashlib.sha256(committed_state).hexdigest()
            results.append(
                {"checkpoint": checkpoint, "format": "legacy-migration",
                 "child_exit": 42, "restart_observations": 2,
                 "result": expected, "source_hashes": original_hashes,
                 "full_state_sha256": state_hash}
            )
    print(
        json.dumps(
            {
                "scope": "process exit inside selected store checkpoint; not an arbitrary write interruption or hardware power loss",
                "results": results,
            }
        )
    )


if __name__ == "__main__":
    main()
