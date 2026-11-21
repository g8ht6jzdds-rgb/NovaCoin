"""Disposable two-node loopback REGTEST exercise; accepts one daemon path."""

import base64
import hashlib
import http.client
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

from test_generation_crash import recovery_fixture


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def hash256(data):
    return hashlib.sha256(hashlib.sha256(data).digest()).digest()


def testnet_child():
    # Frozen v0 TESTNET constants and a one-output height-1 coinbase.
    genesis = bytes.fromhex("7825772a2dd18d4619622b198052a9c82ca17b0f847754f6cb24960df7a7914c")
    transaction = (struct.pack("<i", 1) + b"\x01" + bytes(32) +
                   struct.pack("<I", 0xffffffff) + b"\x02\x01\x01" +
                   struct.pack("<I", 0) + b"\x01" + struct.pack("<q", 5000000000) +
                   b"\x01\x51" + struct.pack("<I", 0))
    merkle = hash256(transaction)
    target = 0x70ffff << (8 * (0x20 - 3))
    for nonce in range(4096):
        header = (struct.pack("<i", 1) + genesis + merkle +
                  struct.pack("<III", 1704154200, 0x2070ffff, nonce))
        block_hash = hash256(header)
        if int.from_bytes(block_hash, "little") <= target:
            return header + b"\x01" + transaction, block_hash.hex(), merkle.hex()
    raise RuntimeError("bounded TESTNET fixture nonce search failed")


def send_testnet_block(port_number, block, expect_disconnect=False):
    magic = struct.pack("<I", 0xdab5bffb)

    def frame(command, payload=b""):
        return (magic + command.encode("ascii").ljust(12, b"\0") +
                struct.pack("<I", len(payload)) + hash256(payload)[:4] + payload)

    def exact(sock, size):
        result = bytearray()
        while len(result) < size:
            chunk = sock.recv(size - len(result))
            if not chunk:
                raise RuntimeError("TESTNET peer closed during handshake")
            result.extend(chunk)
        return bytes(result)

    def receive(sock):
        header = exact(sock, 24)
        require(header[:4] == magic, "unexpected peer magic")
        length = struct.unpack("<I", header[16:20])[0]
        require(length <= 1000000, "oversized peer response")
        payload = exact(sock, length)
        require(hash256(payload)[:4] == header[20:24], "peer checksum mismatch")
        return header[4:16].rstrip(b"\0")

    with socket.create_connection(("127.0.0.1", port_number), timeout=10) as peer:
        peer.settimeout(10)
        require(receive(peer) == b"version", "missing initial version")
        version = struct.pack("<iQqQ", 1, 0, int(time.time()), 0x87654321)
        version += b"\x04test" + struct.pack("<iB", 0, 1)
        peer.sendall(frame("version", version))
        for _ in range(8):
            if receive(peer) == b"verack":
                break
        else:
            raise RuntimeError("missing TESTNET verack")
        peer.sendall(frame("verack") + frame("block", block))
        if expect_disconnect:
            try:
                while True:
                    receive(peer)
            except RuntimeError as error:
                require(str(error) == "TESTNET peer closed during handshake",
                        "unexpected TESTNET peer response")
        else:
            time.sleep(0.2)


def rpc(port_number, method, params=None):
    connection = http.client.HTTPConnection("127.0.0.1", port_number, timeout=30)
    auth = base64.b64encode(b"novacoin:disposable-rpc-password").decode("ascii")
    request = {"jsonrpc": "2.0", "id": 1, "method": method}
    if params is not None:
        request["params"] = params
    try:
        connection.request("POST", "/", json.dumps(request),
                           {"Authorization": "Basic " + auth, "Content-Type": "application/json"})
        response = connection.getresponse()
        payload = json.loads(response.read())
        if response.status != 200 or payload.get("error") is not None:
            raise RuntimeError(f"{method} failed: {payload!r}")
        return payload["result"]
    finally:
        connection.close()


def wait_for(description, predicate, processes, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for process in processes:
            if process.poll() is not None:
                out, err = process.communicate()
                raise RuntimeError(f"daemon exited {process.returncode}: {out!r} {err!r}")
        try:
            if predicate():
                return
        except (OSError, TimeoutError):
            pass
        time.sleep(0.05)
    raise RuntimeError(f"timeout waiting for {description}")


def export_legacy_fixture(source, destination):
    """Offline test fixture: copy verified committed raw images, no writer daemon."""
    store = source / "generations"
    root = (store / "generation.current").read_bytes()
    identity = (source / "network.identity").read_bytes()
    require(len(root) == 85 and root[:8] == b"NVROOT01" and
            len(identity) == 45 and root[8:45] == identity[8:], "invalid fixture root")
    sequence = int.from_bytes(root[45:53], "little")
    require(sequence > 0, "empty fixture root")
    stem = f"generation-{sequence:016x}"
    descriptor = (store / (stem + ".descriptor")).read_bytes()
    require(len(descriptor) == 173 and descriptor[:8] == b"NVGEN002" and
            descriptor[8:53] == root[8:53] and
            hashlib.sha256(descriptor).digest() == root[53:], "invalid fixture descriptor")
    destination.mkdir()
    (destination / "network.identity").write_bytes(identity)
    for extension, filename, offset in (("journal", "blocks.dat", 53),
                                        ("wallet", "wallet.dat", 93)):
        image = (store / (stem + "." + extension)).read_bytes()
        require(len(image) == int.from_bytes(descriptor[offset:offset + 8], "little") and
                hashlib.sha256(image).digest() == descriptor[offset + 8:offset + 40],
                "fixture image mismatch")
        (destination / filename).write_bytes(image)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_durable_regtest_integration.py NOVACOIND")
    executable = Path(sys.argv[1]).resolve(strict=True)
    environment = os.environ.copy()
    environment["NOVACOIN_WALLET_PASSPHRASE"] = "disposable-wallet-passphrase"
    environment["NOVACOIN_RPC_PASSWORD"] = "disposable-rpc-password"
    with recovery_fixture("novacoin-durable-integration-") as temporary:
        root = Path(temporary).resolve(strict=True)
        root.relative_to(Path(tempfile.gettempdir()).resolve())
        ports = set()
        while len(ports) < 4:
            ports.add(port())
        p2p_a, rpc_a, p2p_b, rpc_b = ports
        processes = []

        def start(name, p2p_port, rpc_port, connect=None, durable=True, migrate_from=None,
                  testnet=False, legacy=False):
            args = [str(executable), "--testnet" if testnet else "--regtest", "--name", name,
                    "--datadir", str(root / name), "--logfile", str(root / (name + ".log")),
                    "--p2pport", str(p2p_port), "--rpcport", str(rpc_port)]
            if durable:
                args.append("--durable-testnet" if testnet else "--durable-regtest")
            if legacy:
                args.append("--legacy-regtest")
            if migrate_from is not None:
                args += ["--migrate-testnet-from" if testnet else "--migrate-regtest-from",
                         str(root / migrate_from)]
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
            b = start("b", p2p_b, rpc_b, durable=False)
            require((root / "b" / "generations").is_dir(),
                    "fresh REGTEST did not select durable storage")
            a = start("a", p2p_a, rpc_a, p2p_b)
            address = rpc(rpc_b, "getnewaddress")
            rpc(rpc_a, "generateregtestblock")
            wait_for("first block propagation", lambda: rpc(rpc_b, "getblockchaininfo")["blocks"] == 1,
                     processes)
            txid = rpc(rpc_a, "sendtoaddress", {"address": address, "amount": 1})
            wait_for("transaction propagation", lambda: rpc(rpc_b, "getmempoolinfo")["size"] == 1,
                     processes)
            require(rpc(rpc_b, "getbalances")["unconfirmed"] == 1,
                    "recipient pending balance mismatch")
            sender_chain = rpc(rpc_a, "getblockchaininfo")
            sender_balances = rpc(rpc_a, "getbalances")
            sender_unspent = rpc(rpc_a, "listunspent")
            sender_entry = rpc(rpc_a, "getmempoolentry", {"txid": txid})
            stop(a)
            a = start("a", p2p_a, rpc_a, p2p_b)
            require(rpc(rpc_a, "getblockchaininfo") == sender_chain,
                    "sender chain state not recovered")
            require(rpc(rpc_a, "getbalances") == sender_balances,
                    "sender pending balances not recovered")
            require(rpc(rpc_a, "listunspent") == sender_unspent,
                    "sender pending wallet UTXOs not recovered")
            require(rpc(rpc_a, "getmempoolentry", {"txid": txid}) == sender_entry,
                    "sender pending transaction not recovered")
            stop(b)
            b = start("b", p2p_b, rpc_b, p2p_a)
            require(rpc(rpc_b, "getmempoolinfo")["size"] == 1,
                    "pending transaction not recovered")
            require(rpc(rpc_b, "getbalances")["unconfirmed"] == 1,
                    "recipient pending balance not recovered")
            rpc(rpc_a, "generateregtestblock")
            wait_for("confirmation propagation", lambda: rpc(rpc_b, "getblockchaininfo")["blocks"] == 2,
                     processes)
            chain = rpc(rpc_b, "getblockchaininfo")
            balances = rpc(rpc_b, "getbalances")
            require(balances["confirmed"] == 1 and balances["unconfirmed"] == 0,
                    "confirmed recipient balance mismatch")
            require(rpc(rpc_b, "getmempoolinfo")["size"] == 0,
                    "confirmed transaction remained in mempool")
            stop(b)
            b = start("b", p2p_b, rpc_b)
            require(rpc(rpc_b, "getblockchaininfo") == chain, "chain state not recovered")
            require(rpc(rpc_b, "getbalances") == balances, "wallet balance not recovered")
            stop(a)
            stop(b)

            fixture = start("fixture", p2p_a, rpc_a, durable=False)
            require(isinstance(rpc(rpc_a, "getnewaddress"), str), "fixture address failed")
            rpc(rpc_a, "generateregtestblock")
            legacy_chain = rpc(rpc_a, "getblockchaininfo")
            legacy_balance = rpc(rpc_a, "getbalances")
            require(rpc(rpc_a, "getmempoolinfo")["size"] == 0, "fixture has pending transactions")
            stop(fixture)
            export_legacy_fixture(root / "fixture", root / "legacy")
            for name in ("retired-empty", "legacy"):
                refusal = subprocess.run(
                    [str(executable), "--regtest", "--legacy-regtest", "--name", name,
                     "--datadir", str(root / name), "--logfile", str(root / (name + ".log")),
                     "--p2pport", str(p2p_a), "--rpcport", str(rpc_a)],
                    env=environment, capture_output=True, timeout=15)
                require(refusal.returncode == 2 and b"legacy writer mode is retired" in refusal.stderr,
                        "retired flag enabled a writer")
            require(not (root / "retired-empty").exists(), "retired mode created storage")
            source_paths = [root / "legacy" / "blocks.dat", root / "legacy" / "wallet.dat"]
            source_hashes = [hashlib.sha256(path.read_bytes()).hexdigest() for path in source_paths]
            ambiguous = root / "ambiguous"
            ambiguous.mkdir()
            identity = (root / "legacy" / "network.identity").read_bytes()
            (ambiguous / "network.identity").write_bytes(identity)
            ambiguous_refusal = subprocess.run(
                [str(executable), "--regtest", "--name", "ambiguous",
                 "--datadir", str(ambiguous), "--logfile", str(root / "ambiguous.log"),
                 "--p2pport", str(p2p_a), "--rpcport", str(rpc_a)],
                env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
            require(ambiguous_refusal.returncode != 0 and
                    b"ambiguous data directory" in ambiguous_refusal.stderr,
                    "ambiguous REGTEST directory was not refused")
            require((ambiguous / "network.identity").read_bytes() == identity and
                    not (ambiguous / "generations").exists(),
                    "ambiguous directory was modified")
            migrated = start("migrated", p2p_a, rpc_a, migrate_from="legacy")
            require(rpc(rpc_a, "getblockchaininfo") == legacy_chain,
                    "migrated chain differs from legacy")
            require(rpc(rpc_a, "getbalances") == legacy_balance,
                    "migrated wallet differs from legacy")
            require(rpc(rpc_a, "getmempoolinfo")["size"] == 0,
                    "migration unexpectedly retained legacy mempool")
            rpc(rpc_a, "generateregtestblock")
            migrated_chain = rpc(rpc_a, "getblockchaininfo")
            migrated_balances = rpc(rpc_a, "getbalances")
            require(migrated_chain["blocks"] == legacy_chain["blocks"] + 1,
                    "migrated node did not commit a new block")
            stop(migrated)
            # An existing durable directory must select the coordinator even
            # when the initialization flag is omitted on a later restart.
            migrated = start("migrated", p2p_a, rpc_a, durable=False)
            require(rpc(rpc_a, "getblockchaininfo") == migrated_chain,
                    "post-migration chain not recovered")
            require(rpc(rpc_a, "getbalances") == migrated_balances,
                    "post-migration wallet not recovered")
            stop(migrated)
            legacy_refusal = subprocess.run(
                [str(executable), "--regtest", "--legacy-regtest", "--name", "migrated",
                 "--datadir", str(root / "migrated"),
                 "--logfile", str(root / "migrated.log"),
                 "--p2pport", str(p2p_a), "--rpcport", str(rpc_a)],
                env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
            require(legacy_refusal.returncode != 0 and
                    b"legacy writer mode is retired" in legacy_refusal.stderr,
                    "legacy mode did not refuse existing durable state")
            conflicting_wallet = root / "migrated" / "wallet.dat"
            conflicting_wallet.write_bytes(b"disposable conflicting legacy wallet")
            refusal = subprocess.run(
                [str(executable), "--regtest", "--name", "migrated",
                 "--datadir", str(root / "migrated"),
                 "--logfile", str(root / "migrated.log"),
                 "--p2pport", str(p2p_a), "--rpcport", str(rpc_a)],
                env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
            require(refusal.returncode != 0, "mixed legacy and durable state was accepted")
            require(b"legacy data requires offline migration" in refusal.stderr,
                    "mixed-state refusal did not fail closed before startup")
            require(conflicting_wallet.read_bytes() == b"disposable conflicting legacy wallet",
                    "refusal modified conflicting legacy wallet")
            require([hashlib.sha256(path.read_bytes()).hexdigest() for path in source_paths] ==
                    source_hashes, "migration modified legacy journal or wallet")
            testnet = start("testnet", p2p_a, rpc_a, testnet=True, durable=False)
            require((root / "testnet" / "generations").is_dir() and
                    not (root / "testnet" / "wallet.dat").exists(),
                    "fresh TESTNET selected independent legacy writers")
            testnet_chain = rpc(rpc_a, "getblockchaininfo")
            require(testnet_chain["blocks"] == 0, "TESTNET genesis height mismatch")
            testnet_address = rpc(rpc_a, "getnewaddress")
            require(isinstance(testnet_address, str) and testnet_address,
                    "TESTNET address creation failed")
            testnet_balances = rpc(rpc_a, "getbalances")
            stop(testnet)
            testnet = start("testnet", p2p_a, rpc_a, testnet=True, durable=False)
            require(rpc(rpc_a, "getblockchaininfo") == testnet_chain,
                    "TESTNET chain not recovered")
            require(rpc(rpc_a, "getbalances") == testnet_balances,
                    "TESTNET wallet not recovered")
            testnet_b = start("testnet-b", p2p_b, rpc_b, connect=p2p_a, testnet=True)
            wait_for("TESTNET loopback peer", lambda: rpc(rpc_a, "getpeerinfo")["count"] > 0,
                     processes)
            block, block_hash, coinbase_id = testnet_child()
            invalid = bytearray(block)
            invalid[36] ^= 1  # Break the header's Merkle commitment.
            send_testnet_block(p2p_a, invalid, expect_disconnect=True)
            require(rpc(rpc_a, "getblockchaininfo")["blocks"] == 0,
                    "invalid TESTNET block changed active state")
            send_testnet_block(p2p_a, block)
            wait_for("TESTNET child acceptance",
                     lambda: rpc(rpc_a, "getblockchaininfo")["bestblockhash"] == block_hash,
                     processes)
            wait_for("TESTNET child propagation",
                     lambda: rpc(rpc_b, "getblockchaininfo")["bestblockhash"] == block_hash,
                     processes)
            outpoint = {"txid": coinbase_id, "vout": 0}
            coin_a = rpc(rpc_a, "getutxo", outpoint)
            require(rpc(rpc_b, "getutxo", outpoint) == coin_a,
                    "TESTNET peer coin mismatch")
            stop(testnet)
            testnet = start("testnet", p2p_a, rpc_a, testnet=True, durable=False)
            require(rpc(rpc_a, "getblockchaininfo")["bestblockhash"] == block_hash,
                    "TESTNET child tip not recovered")
            require(rpc(rpc_a, "getutxo", outpoint) == coin_a,
                    "TESTNET child coin not recovered")
            stop(testnet)
            stop(testnet_b)
            legacy_testnet = start("testnet-export", p2p_a, rpc_a,
                                   durable=False, testnet=True)
            require(isinstance(rpc(rpc_a, "getnewaddress"), str),
                    "legacy TESTNET address creation failed")
            send_testnet_block(p2p_a, block)
            wait_for("legacy TESTNET child acceptance",
                     lambda: rpc(rpc_a, "getblockchaininfo")["bestblockhash"] == block_hash,
                     processes)
            migration_chain = rpc(rpc_a, "getblockchaininfo")
            migration_balances = rpc(rpc_a, "getbalances")
            migration_coin = rpc(rpc_a, "getutxo", outpoint)
            require(rpc(rpc_a, "getmempoolinfo")["size"] == 0, "TESTNET fixture pending pool")
            stop(legacy_testnet)
            export_legacy_fixture(root / "testnet-export", root / "legacy-testnet")
            testnet_sources = [root / "legacy-testnet" / name
                               for name in ("network.identity", "wallet.dat", "blocks.dat")]
            testnet_hashes = [hashlib.sha256(path.read_bytes()).hexdigest()
                              for path in testnet_sources]
            migrated_testnet = start("migrated-testnet", p2p_a, rpc_a,
                                     testnet=True, migrate_from="legacy-testnet")
            require(rpc(rpc_a, "getblockchaininfo") == migration_chain and
                    rpc(rpc_a, "getbalances") == migration_balances and
                    rpc(rpc_a, "getutxo", outpoint) == migration_coin,
                    "TESTNET migration changed chain, wallet or coin")
            stop(migrated_testnet)
            migrated_testnet = start("migrated-testnet", p2p_a, rpc_a, testnet=True)
            require(rpc(rpc_a, "getblockchaininfo") == migration_chain and
                    rpc(rpc_a, "getutxo", outpoint) == migration_coin,
                    "migrated TESTNET state not recovered")
            stop(migrated_testnet)
            require([hashlib.sha256(path.read_bytes()).hexdigest()
                     for path in testnet_sources] == testnet_hashes,
                    "TESTNET migration modified source images")
            # Network selection still rejects incompatible durable flags before storage/listeners.
            for network, option in (("--testnet", "--durable-regtest"),
                                    ("--regtest", "--durable-testnet")):
                refusal = subprocess.run(
                    [str(executable), network, option, "--name", "mismatch",
                     "--datadir", str(root / "mismatch"), "--logfile", str(root / "mismatch.log"),
                     "--p2pport", str(p2p_a), "--rpcport", str(rpc_a)],
                    env=environment, capture_output=True, timeout=15)
                require(refusal.returncode == 2 and b"conflicts" in refusal.stderr,
                        "wrong-network startup was accepted")
                require(not (root / "mismatch").exists(), "wrong-network startup wrote storage")
            print(json.dumps({"result": "PASS", "height": chain["blocks"], "txid": txid,
                              "scope": "loopback REGTEST migration and TESTNET block propagation"}))
        finally:
            for process in processes:
                if process.poll() is None:
                    process.terminate()
            for process in processes:
                process.wait(timeout=10)


if __name__ == "__main__":
    main()
