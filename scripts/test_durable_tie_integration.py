"""Disposable equal-work branch selection and recovery test."""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from test_durable_regtest_integration import port, require, rpc, wait_for
from test_generation_crash import recovery_fixture


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_durable_tie_integration.py NOVACOIND INSPECTOR")
    executable = Path(sys.argv[1]).resolve(strict=True)
    inspector = Path(sys.argv[2]).resolve(strict=True)
    environment = os.environ.copy()
    environment["NOVACOIN_WALLET_PASSPHRASE"] = "disposable-wallet-passphrase"
    environment["NOVACOIN_RPC_PASSWORD"] = "disposable-rpc-password"
    with recovery_fixture("novacoin-durable-reorg-") as temporary:
        root = Path(temporary).resolve(strict=True)
        root.relative_to(Path(tempfile.gettempdir()).resolve())
        (root / "OWNED-TEST-FIXTURE").write_bytes(b"NOVA-DURABLE-REORG-FIXTURE\n")
        ports = set()
        while len(ports) < 4:
            ports.add(port())
        p2p_a, rpc_a, p2p_b, rpc_b = ports
        processes = []

        def start(name, p2p_port, rpc_port, connect=None):
            args = [str(executable), "--regtest", "--durable-regtest", "--name", name,
                    "--datadir", str(root / name), "--logfile", str(root / (name + ".log")),
                    "--p2pport", str(p2p_port), "--rpcport", str(rpc_port)]
            if connect is not None:
                args += ["--connect", f"127.0.0.1:{connect}"]
            process = subprocess.Popen(args, env=environment, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE)
            processes.append(process)
            wait_for(name + " RPC", lambda: rpc(rpc_port, "getblockchaininfo"), processes)
            return process

        def stop(process):
            process.terminate()
            process.wait(timeout=10)
            processes.remove(process)

        try:
            b = start("b", p2p_b, rpc_b)
            a = start("a", p2p_a, rpc_a, p2p_b)
            rpc(rpc_a, "generateregtestblock")
            wait_for("common block", lambda: rpc(rpc_b, "getblockchaininfo")["blocks"] == 1,
                     processes)
            rpc(rpc_a, "disconnectpeers")
            rpc(rpc_b, "disconnectpeers")
            wait_for("partition", lambda: rpc(rpc_a, "getpeerinfo")["count"] == 0 and
                     rpc(rpc_b, "getpeerinfo")["count"] == 0, processes)
            tip_a = rpc(rpc_a, "generateregtestblock")
            tip_b = rpc(rpc_b, "generateregtestblock")
            require(tip_a != tip_b, "competing block IDs unexpectedly equal")
            work_a = rpc(rpc_a, "getnodehealth")["chainwork"]
            work_b = rpc(rpc_b, "getnodehealth")["chainwork"]
            require(work_a and work_a == work_b, "branches do not have equal reported work")
            expected = min(tip_a, tip_b)
            rpc(rpc_a, "connectpeer", {"host": "127.0.0.1", "port": p2p_b})
            wait_for("equal-work convergence", lambda:
                     rpc(rpc_a, "getblockchaininfo")["bestblockhash"] == expected and
                     rpc(rpc_b, "getblockchaininfo")["bestblockhash"] == expected,
                     processes, timeout=90)
            require(rpc(rpc_a, "getnodehealth")["chainwork"] == work_a, "work changed at tie")
            wallet_a, wallet_b = rpc(rpc_a, "getbalances"), rpc(rpc_b, "getbalances")
            stop(a)
            stop(b)
            snapshots = []
            for name in ("a", "b"):
                result = subprocess.run([str(inspector), str(root), name], env=environment,
                                        capture_output=True, timeout=180)
                require(result.returncode == 0, f"{name} replay inspector failed: {result.returncode}")
                snapshots.append(result.stdout)
            require(snapshots[0] == snapshots[1], "equal-work recovered UTXO sets differ")
            a = start("a", p2p_a, rpc_a)
            b = start("b", p2p_b, rpc_b)
            require(rpc(rpc_a, "getblockchaininfo")["bestblockhash"] == expected,
                    "node a did not recover winning tip")
            require(rpc(rpc_b, "getblockchaininfo")["bestblockhash"] == expected,
                    "node b did not recover winning tip")
            require(rpc(rpc_a, "getbalances") == wallet_a and
                    rpc(rpc_b, "getbalances") == wallet_b, "wallet state changed on recovery")
            print(json.dumps({"result": "PASS", "tips": [tip_a, tip_b], "winner": expected,
                              "chainwork": work_a, "utxo_sha256": hashlib.sha256(snapshots[0]).hexdigest(),
                              "utxo_summary": snapshots[0].decode("ascii").splitlines()[-1]}))
        finally:
            for process in processes:
                if process.poll() is None:
                    process.terminate()
            for process in processes:
                process.wait(timeout=10)


if __name__ == "__main__":
    main()
