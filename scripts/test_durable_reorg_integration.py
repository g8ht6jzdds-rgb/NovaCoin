"""Disposable two-node competing-branch REGTEST exercise."""

import hashlib
import json
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile

from test_durable_regtest_integration import port, require, rpc, wait_for
from test_generation_crash import recovery_fixture


def main():
    if len(sys.argv) not in (3, 4, 5):
        raise SystemExit("usage: test_durable_reorg_integration.py NOVACOIND INSPECTOR "
                         "[1|10|100 [--diagnostic-slow-host]]")
    depth = int(sys.argv[3]) if len(sys.argv) >= 4 else 1
    if len(sys.argv) == 5 and sys.argv[4] != "--diagnostic-slow-host":
        raise SystemExit("unknown diagnostic profile")
    diagnostic = len(sys.argv) == 5
    reorg_seconds = 600 if diagnostic else 180
    if depth not in (1, 10, 100):
        raise SystemExit("depth must be 1, 10, or 100")
    executable = Path(sys.argv[1]).resolve(strict=True)
    inspector = Path(sys.argv[2]).resolve(strict=True)
    environment = os.environ.copy()
    environment["NOVACOIN_WALLET_PASSPHRASE"] = "disposable-wallet-passphrase"
    environment["NOVACOIN_RPC_PASSWORD"] = "disposable-rpc-password"
    with recovery_fixture("novacoin-durable-reorg-") as temporary:
        root = Path(temporary).resolve(strict=True)
        root.relative_to(Path(tempfile.gettempdir()).resolve())
        (root / "OWNED-TEST-FIXTURE").write_bytes(b"NOVA-DURABLE-REORG-FIXTURE\n")
        def require_space():
            require(shutil.disk_usage(root).free >= 512 * 1024 * 1024,
                    "test resource guard: less than 512 MiB host free space")

        require_space()
        ports = set()
        while len(ports) < 4:
            ports.add(port())
        p2p_a, rpc_a, p2p_b, rpc_b = ports
        processes = []
        launches = []

        def start(name, p2p_port, rpc_port, connect=None):
            args = [str(executable), "--regtest", "--durable-regtest", "--name", name,
                    "--datadir", str(root / name), "--logfile", str(root / (name + ".log")),
                    "--p2pport", str(p2p_port), "--rpcport", str(rpc_port)]
            if connect is not None:
                args += ["--connect", f"127.0.0.1:{connect}"]
            launch = len(launches)
            with (root / f"daemon-{launch}.stdout").open("wb") as stdout, \
                    (root / f"daemon-{launch}.stderr").open("wb") as stderr:
                process = subprocess.Popen(args, env=environment, stdout=stdout, stderr=stderr)
            launches.append({"process": process, "args": args, "cleanup_exit": None})
            processes.append(process)
            wait_for(name + " RPC", lambda: rpc(rpc_port, "getblockchaininfo"), processes)
            return process

        def stop(process):
            for launch in launches:
                if launch["process"] is process:
                    launch["cleanup_exit"] = process.poll()
            process.terminate()
            process.wait(timeout=10)
            processes.remove(process)

        def inspect(name):
            result = subprocess.run([str(inspector), str(root), name], env=environment,
                                    capture_output=True, timeout=180)
            require(result.returncode == 0,
                    f"{name} UTXO replay failed: {result.returncode} {result.stderr!r}")
            return result.stdout

        try:
            b = start("b", p2p_b, rpc_b)
            a = start("a", p2p_a, rpc_a, p2p_b)
            address = rpc(rpc_b, "getnewaddress")
            rpc(rpc_a, "generateregtestblock")
            wait_for("common block", lambda: rpc(rpc_b, "getblockchaininfo")["blocks"] == 1,
                     processes)
            txid = rpc(rpc_a, "sendtoaddress", {"address": address, "amount": 1})
            wait_for("pending payment", lambda: rpc(rpc_b, "getmempoolinfo")["size"] == 1,
                     processes)
            rpc(rpc_a, "generateregtestblock")
            wait_for("confirmed payment", lambda: rpc(rpc_b, "getblockchaininfo")["blocks"] == 2,
                     processes)
            payment = [coin for coin in rpc(rpc_b, "listunspent") if coin["txid"] == txid]
            require(len(payment) == 1 and payment[0]["amount"] == 1 and payment[0]["confirmed"],
                    "payment not confirmed in recipient wallet")
            outpoint = {"txid": txid, "vout": payment[0]["vout"]}
            coin_before = rpc(rpc_a, "getutxo", outpoint)
            rpc(rpc_a, "disconnectpeers")
            rpc(rpc_b, "disconnectpeers")
            wait_for("partition", lambda: rpc(rpc_a, "getpeerinfo")["count"] == 0 and
                     rpc(rpc_b, "getpeerinfo")["count"] == 0, processes)
            orphan_payment = rpc(rpc_a, "sendtoaddress", {"address": address, "amount": 2})
            pending_wallet = rpc(rpc_a, "getbalances")
            orphan = rpc(rpc_a, "generateregtestblock")
            orphan_outpoint = {"txid": orphan_payment, "vout": 0}
            require(rpc(rpc_a, "getutxo", orphan_outpoint)["amount"] == 2,
                    "orphan-branch payment was not created")
            for _ in range(depth - 1):
                require_space()
                orphan = rpc(rpc_a, "generateregtestblock")
            for _ in range(depth):
                require_space()
                rpc(rpc_b, "generateregtestblock")
            winning_tip = rpc(rpc_b, "generateregtestblock")
            winning_wallet = rpc(rpc_b, "getbalances")
            require(rpc(rpc_a, "getblockchaininfo")["blocks"] == 2 + depth, "short branch height")
            require(rpc(rpc_b, "getblockchaininfo")["blocks"] == 3 + depth, "winning branch height")
            stop(a)
            stop(b)
            require(inspect("a") != inspect("b"), "competing UTXO sets unexpectedly match")
            b = start("b", p2p_b, rpc_b)
            a = start("a", p2p_a, rpc_a)
            rpc(rpc_a, "connectpeer", {"host": "127.0.0.1", "port": p2p_b})
            wait_for("reorganization", lambda: rpc(rpc_a, "getblockchaininfo")["bestblockhash"] == winning_tip,
                     processes, timeout=reorg_seconds)
            chain = rpc(rpc_a, "getblockchaininfo")
            peer_chain = rpc(rpc_b, "getblockchaininfo", timeout=120 if depth == 100 else 30)
            require(chain == peer_chain, "active tips differ")
            require(chain["bestblockhash"] != orphan, "orphan remains active")
            require(rpc(rpc_a, "getnodehealth")["chainwork"] ==
                    rpc(rpc_b, "getnodehealth")["chainwork"], "chainwork mismatch")
            require(rpc(rpc_a, "getbalances") == pending_wallet,
                    "wallet differs from pre-orphan pending state")
            rpc(rpc_a, "getmempoolentry", {"txid": orphan_payment})
            require(rpc(rpc_b, "getbalances") == winning_wallet, "winning wallet changed")
            try:
                rpc(rpc_a, "getutxo", orphan_outpoint)
            except RuntimeError as error:
                require("UTXO not found" in str(error), "unexpected orphan lookup failure")
            else:
                raise RuntimeError("orphan-branch payment remains in UTXO set")
            require(rpc(rpc_a, "getutxo", outpoint) == coin_before, "payment UTXO changed")
            require(rpc(rpc_b, "getutxo", outpoint) == coin_before, "peer payment UTXO differs")
            a_balance = rpc(rpc_a, "getbalances")
            stop(a)
            a = start("a", p2p_a, rpc_a)
            require(rpc(rpc_a, "getblockchaininfo") == chain, "reorganized tip not recovered")
            require(rpc(rpc_a, "getbalances") == a_balance, "reorganized wallet not recovered")
            require(rpc(rpc_a, "getutxo", outpoint) == coin_before, "payment UTXO not recovered")
            rpc(rpc_a, "getmempoolentry", {"txid": orphan_payment})
            try:
                rpc(rpc_a, "getutxo", orphan_outpoint)
            except RuntimeError as error:
                require("UTXO not found" in str(error), "unexpected recovered orphan lookup failure")
            else:
                raise RuntimeError("orphan-branch UTXO reappeared after restart")
            stop(a)
            stop(b)
            snapshots = [inspect("a"), inspect("b")]
            require(snapshots[0] == snapshots[1], "complete recovered UTXO sets differ")
            print(json.dumps({"result": "PASS", "height": chain["blocks"],
                              "orphan": orphan, "tip": winning_tip, "payment": txid,
                              "orphan_payment": orphan_payment,
                              "utxo_sha256": hashlib.sha256(snapshots[0]).hexdigest(),
                              "utxo_summary": snapshots[0].decode("ascii").splitlines()[-1],
                              "depth": depth, "scope": "loopback REGTEST",
                              "reorganization_budget_seconds": reorg_seconds,
                              "diagnostic_only": diagnostic}))
        finally:
            for process in processes:
                for launch in launches:
                    if launch["process"] is process:
                        launch["cleanup_exit"] = process.poll()
                if process.poll() is None:
                    process.terminate()
            for process in processes:
                process.wait(timeout=10)
            (root / "process-results.json").write_text(json.dumps([
                {"args": item["args"], "pid": item["process"].pid,
                 "exit_before_cleanup": item["cleanup_exit"],
                 "final_exit": item["process"].returncode}
                for item in launches], indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
