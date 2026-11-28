"""Local durable TESTNET RPC role/configuration regression; no public endpoints."""

import base64
import http.client
import json
import os
from pathlib import Path
import subprocess
import sys

from test_durable_regtest_integration import port, require, rpc, wait_for
from test_generation_crash import recovery_fixture


def main():
    executable = Path(sys.argv[1]).resolve(strict=True)
    environment = os.environ.copy()
    for name in ("NOVACOIN_FAUCET_RPC_PASSWORD", "NOVACOIN_FAUCET_MAX_PAYOUT",
                 "NOVACOIN_EXPLORER_RPC_PASSWORD"):
        environment.pop(name, None)
    environment.update(NOVACOIN_WALLET_PASSPHRASE="disposable-wallet-passphrase",
                       NOVACOIN_RPC_PASSWORD="disposable-rpc-password")
    with recovery_fixture("novacoin-durable-integration-") as temporary:
        root = Path(temporary)
        p2p, rpc_port = port(), port()
        while p2p == rpc_port:
            rpc_port = port()

        def arguments(directory):
            return [str(executable), "--testnet", "--durable-testnet", "--name", "roles",
                    "--datadir", str(directory), "--logfile", str(root / "roles.log"),
                    "--p2pport", str(p2p), "--rpcport", str(rpc_port)]

        invalid = [{"NOVACOIN_FAUCET_RPC_PASSWORD": "disposable-faucet"},
                   {"NOVACOIN_FAUCET_MAX_PAYOUT": "10"}]
        invalid += [{"NOVACOIN_FAUCET_RPC_PASSWORD": "disposable-faucet",
                     "NOVACOIN_FAUCET_MAX_PAYOUT": value}
                    for value in ("0", "-1", "10x", "9223372036854775808")]
        for index, values in enumerate(invalid):
            directory = root / ("invalid-" + str(index))
            result = subprocess.run(arguments(directory), env={**environment, **values},
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
            require(result.returncode == 2 and b"invalid TESTNET faucet RPC configuration" in
                    result.stderr, "invalid faucet configuration did not fail closed")
            require(not directory.exists(), "invalid configuration opened persistent state")

        environment.update(NOVACOIN_FAUCET_RPC_PASSWORD="disposable-faucet",
                           NOVACOIN_FAUCET_MAX_PAYOUT="10")
        directory = root / "valid"
        process = subprocess.Popen(arguments(directory), env=environment,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            wait_for("durable faucet RPC", lambda: rpc(rpc_port, "getblockchaininfo"), [process])
            address = rpc(rpc_port, "getnewaddress")
            selector = directory / "generations" / "generation.current"
            before = selector.read_bytes()
            balance = rpc(rpc_port, "getbalances")
            for amount, expected in ((0, -32602), (11, -32602), (1, -32004), (10, -32004)):
                connection = http.client.HTTPConnection("127.0.0.1", rpc_port, timeout=10)
                try:
                    auth = base64.b64encode(b"faucet:disposable-faucet").decode("ascii")
                    connection.request("POST", "/", json.dumps({"jsonrpc": "2.0", "id": 1,
                        "method": "faucetpay", "params": {"address": address, "amount": amount}}),
                        {"Authorization": "Basic " + auth, "Content-Type": "application/json"})
                    response = connection.getresponse()
                    payload = json.loads(response.read())
                    require(response.status == 200 and payload["error"]["code"] == expected,
                            "faucet role/limit or unfunded rejection mismatch: " + str(payload))
                finally:
                    connection.close()
                require(selector.read_bytes() == before and rpc(rpc_port, "getbalances") == balance
                        and rpc(rpc_port, "getmempoolinfo")["size"] == 0,
                        "rejected faucet request published a transition")
        finally:
            process.terminate()
            process.communicate(timeout=10)
        print("PASS: durable faucet configuration, bounded role admission, rejection non-mutation")


if __name__ == "__main__":
    main()
