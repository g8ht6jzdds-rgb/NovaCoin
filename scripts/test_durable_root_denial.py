"""Windows native selector-replacement denial through daemon writer adapters."""

import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from test_durable_regtest_integration import port, require, rpc, wait_for
from test_generation_crash import recovery_fixture


def main():
    if os.name != "nt" or len(sys.argv) != 2:
        raise SystemExit("requires Windows and one NOVACOIND path; unsupported is not a pass")
    daemon = Path(sys.argv[1]).resolve(strict=True)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                  ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD,
                                  wintypes.HANDLE]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    environment = os.environ.copy()
    environment["NOVACOIN_WALLET_PASSPHRASE"] = "disposable-wallet-passphrase"
    environment["NOVACOIN_RPC_PASSWORD"] = "disposable-rpc-password"
    with recovery_fixture("novacoin-durable-integration-") as temporary:
        root = Path(temporary)
        require(shutil.disk_usage(root).free >= 512 * 1024 * 1024, "insufficient test space")
        p2p, http = port(), port()
        require(p2p != http, "port collision")
        processes = []
        records = []
        selector = root / "node" / "generations" / "generation.current"

        def start():
            args = [str(daemon), "--regtest", "--name", "root-denial",
                    "--datadir", str(root / "node"), "--logfile", str(root / "node.log"),
                    "--p2pport", str(p2p), "--rpcport", str(http)]
            index = len(records)
            with (root / f"daemon-{index}.stdout").open("wb") as stdout, \
                    (root / f"daemon-{index}.stderr").open("wb") as stderr:
                child = subprocess.Popen(args, env=environment, stdout=stdout, stderr=stderr)
            processes.append(child)
            records.append({"args": args, "pid": child.pid})
            wait_for("root denial daemon", lambda: rpc(http, "getblockchaininfo"), processes)
            return child

        def stop(child):
            child.terminate()  # This owned process only; models process exit, not power loss.
            child.wait(timeout=10)
            processes.remove(child)

        def snapshot():
            return {method: rpc(http, method) for method in
                    ("getblockchaininfo", "getbalances", "listunspent", "getmempoolinfo")}

        try:
            child = start()
            address = rpc(http, "getnewaddress")
            rpc(http, "generateregtestblock")
            before = snapshot()
            root_bytes = selector.read_bytes()
            # A real OS handle denies DELETE sharing; reads remain permitted.
            handle = kernel.CreateFileW(str(selector), 0x80000000, 1, None, 3, 0x80, None)
            require(handle != ctypes.c_void_p(-1).value, "cannot acquire selector denial handle")
            try:
                probe = kernel.CreateFileW(str(selector), 0x10000, 7, None, 3, 0x80, None)
                error = ctypes.get_last_error()
                if probe != ctypes.c_void_p(-1).value:
                    kernel.CloseHandle(probe)
                    raise RuntimeError("DELETE access was not denied")
                require(error == 32, f"unexpected native denial code {error}")
                for method, params, code in (("getnewaddress", None, -32003),
                                             ("sendtoaddress", {"address": address, "amount": 1}, -32004),
                                             ("generateregtestblock", None, -32004)):
                    try:
                        rpc(http, method, params)
                    except RuntimeError as failure:
                        text = str(failure)
                        require(str(code) in text and "unavailable" not in text and
                                "may have committed" not in text,
                                f"unexpected result classification: {text}")
                    else:
                        raise RuntimeError(f"{method} accepted despite denied root replacement")
                    require(selector.read_bytes() == root_bytes, "rejection changed selector")
                    require(snapshot() == before, "rejection published a state transition")
            finally:
                require(bool(kernel.CloseHandle(handle)), "selector handle close failed")
            stop(child)
            child = start()
            require(snapshot() == before and selector.read_bytes() == root_bytes,
                    "rejected transition appeared after restart")
            payment = rpc(http, "sendtoaddress", {"address": address, "amount": 1})
            require(rpc(http, "getmempoolinfo")["size"] == 1, "retry did not admit payment")
            rpc(http, "generateregtestblock")
            expected = snapshot()
            require(expected["getblockchaininfo"]["blocks"] == 2 and
                    expected["getmempoolinfo"]["size"] == 0, "retry did not confirm payment")
            committed = selector.read_bytes()
            require(committed != root_bytes, "successful retry did not publish")
            for _ in range(2):
                stop(child)
                child = start()
                require(snapshot() == expected and selector.read_bytes() == committed,
                        "committed retry did not survive repeated restart")
            print(json.dumps({"result": "PASS", "native_delete_denial": 32,
                              "rejected_writers": 3, "committed_restarts": 2,
                              "selector_sha256": hashlib.sha256(committed).hexdigest(),
                              "payment": payment, "power_loss_evidence": False}))
        finally:
            for child in processes:
                if child.poll() is None:
                    child.terminate()
            for child in processes:
                child.wait(timeout=10)
            (root / "process-records.json").write_text(json.dumps(records, indent=2),
                                                        encoding="utf-8")


if __name__ == "__main__":
    main()
