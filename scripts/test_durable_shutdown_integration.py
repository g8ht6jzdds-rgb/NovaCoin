"""Graceful shutdown of owned loopback daemons; isolated console on Windows."""

import ctypes
import json
import os
from pathlib import Path
import signal
import subprocess
import sys

from test_durable_regtest_integration import port, require, rpc, wait_for
from test_generation_crash import recovery_fixture


def worker(executable):
    environment = os.environ.copy()
    environment.update(NOVACOIN_WALLET_PASSPHRASE="disposable-wallet-passphrase",
                       NOVACOIN_RPC_PASSWORD="disposable-rpc-password")
    for name in ("NOVACOIN_FAUCET_RPC_PASSWORD", "NOVACOIN_FAUCET_MAX_PAYOUT",
                 "NOVACOIN_EXPLORER_RPC_PASSWORD"):
        environment.pop(name, None)
    with recovery_fixture("novacoin-durable-integration-") as temporary:
        root = Path(temporary)
        p2p, rpc_port = port(), port()
        while p2p == rpc_port:
            rpc_port = port()
        arguments = [str(executable), "--regtest", "--name", "shutdown",
                     "--datadir", str(root / "node"), "--logfile", str(root / "node.log"),
                     "--p2pport", str(p2p), "--rpcport", str(rpc_port)]
        process = None

        def start():
            owned = subprocess.Popen(arguments, env=environment, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE,
                                     creationflags=subprocess.CREATE_NEW_PROCESS_GROUP
                                     if os.name == "nt" else 0)
            return owned

        def graceful(owned):
            require(owned.poll() is None, "daemon exited before shutdown request")
            if os.name == "nt":
                # Only the child-created process group receives CTRL_BREAK.
                kernel = ctypes.WinDLL("kernel32", use_last_error=True)
                generate = kernel.GenerateConsoleCtrlEvent
                generate.argtypes = [ctypes.c_ulong, ctypes.c_ulong]
                generate.restype = ctypes.c_int
                if not generate(1, owned.pid):
                    raise ctypes.WinError(ctypes.get_last_error())
            else:
                owned.send_signal(signal.SIGTERM)
            out, err = owned.communicate(timeout=15)
            require(owned.returncode == 0,
                    "graceful shutdown failed: " + repr((owned.returncode, out, err)))

        try:
            process = start()
            wait_for("shutdown node", lambda: rpc(rpc_port, "getblockchaininfo"), [process])
            address = rpc(rpc_port, "getnewaddress")
            rpc(rpc_port, "generateregtestblock")
            txid = rpc(rpc_port, "sendtoaddress", {"address": address, "amount": 1})
            before = {method: rpc(rpc_port, method) for method in
                      ("getblockchaininfo", "getbalances", "listunspent", "getmempoolinfo")}
            pending = rpc(rpc_port, "getmempoolentry", {"txid": txid})
            selector = root / "node" / "generations" / "generation.current"
            committed = selector.read_bytes()
            for _ in range(3):
                graceful(process)
                require(selector.read_bytes() == committed, "shutdown rewrote committed authority")
                process = start()
                wait_for("shutdown restart", lambda: rpc(rpc_port, "getblockchaininfo"), [process])
                for method, expected in before.items():
                    require(rpc(rpc_port, method) == expected, "restart mismatch: " + method)
                require(rpc(rpc_port, "getmempoolentry", {"txid": txid}) == pending,
                        "restart changed pending transaction")
            graceful(process)
            print(json.dumps({"result": "PASS", "restarts": 3,
                              "signal": "CTRL_BREAK" if os.name == "nt" else "SIGTERM",
                              "scope": "owned loopback daemon graceful shutdown and recovery"}))
        finally:
            if process is not None and process.poll() is None:
                process.terminate()
                process.communicate(timeout=15)


def main():
    executable = Path(sys.argv[1]).resolve(strict=True)
    if os.name == "nt" and "--worker" not in sys.argv[2:]:
        # A separate hidden console lets the worker signal only its own daemon
        # group without attaching to or signalling the user's terminal.
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = subprocess.SW_HIDE
        result = subprocess.run([sys.executable, "-B", __file__, str(executable), "--worker"],
                                creationflags=subprocess.CREATE_NEW_CONSOLE,
                                startupinfo=startup, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True, timeout=150)
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        raise SystemExit(result.returncode)
    worker(executable)


if __name__ == "__main__":
    main()
