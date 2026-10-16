#!/usr/bin/env python3
"""Bounded, loopback-only REGTEST bootstrap startup integration test."""

import base64
import hashlib
import http.client
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time


IDENTITY = (
    "version=1\nnetwork=regtest\nmagic=dab5bffa\n"
    "genesis=c974d11a5276ca7eb69b1ec0a8062fb47b49c02f5ac726532483d090ac53eb79\n"
)
PASSWORD = "bootstrap-test-rpc-password"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def write(path, content):
    path.write_bytes(content.encode("utf-8"))


def reserve_ports(count):
    # Hold all reservations together to prevent duplicates; a later bind race
    # fails the test rather than falling back to an alternate peer or port.
    sockets = []
    try:
        for _ in range(count):
            listener = socket.socket()
            sockets.append(listener)
            listener.bind(("127.0.0.1", 0))
        return [item.getsockname()[1] for item in sockets]
    finally:
        for item in sockets:
            item.close()


class Sentinel:
    def __init__(self):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(16)
        self.listener.settimeout(0.05)
        self.port = self.listener.getsockname()[1]
        self.count = 0
        self.error = None
        self.ready = threading.Event()
        self.stop = threading.Event()
        self.started = time.monotonic()
        self.thread = threading.Thread(target=self.observe, daemon=True)
        self.thread.start()
        require(self.ready.wait(1), "sentinel readiness timeout")

    def observe(self):
        self.ready.set()
        try:
            while not self.stop.is_set():
                require(time.monotonic() - self.started < 30, "sentinel deadline")
                try:
                    connection, _ = self.listener.accept()
                except socket.timeout:
                    continue
                connection.close()
                self.count += 1
                require(self.count <= 16, "unexpected connection flood")
        except Exception as error:
            self.error = str(error)

    def finish(self):
        # Continue accepting through the post-exit drain, not just a snapshot.
        time.sleep(0.2)
        self.stop.set()
        self.thread.join(1)
        self.listener.close()
        require(not self.thread.is_alive(), "sentinel did not stop")
        require(self.error is None, f"sentinel error: {self.error}")
        return {"ready": True, "accepted": self.count,
                "interval_seconds": time.monotonic() - self.started}


def verify_evidence(directory):
    names = {"events.json", "bootstrap-result.txt", "canonical-regtest.conf"}
    for case in ("network", "genesis", "magic", "late-genesis"):
        names.update({f"rejected-{case}.conf", f"rejected-{case}.txt"})
    require({path.name for path in directory.iterdir()} == names | {"SHA256SUMS"},
            "incomplete or unexpected evidence files")
    raw = (directory / "SHA256SUMS").read_bytes()
    require(b"\r" not in raw and raw.endswith(b"\n"), "manifest is not LF-only")
    entries = raw.decode("ascii").splitlines()
    require(entries == sorted(entries, key=lambda line: line.split("  ")[1]),
            "manifest is not path sorted")
    observed = set()
    for line in entries:
        digest, name = line.split("  ")
        require(name in names and name not in observed, "unsafe or duplicate manifest entry")
        observed.add(name)
        path = directory / name
        require(not path.is_symlink() and path.stat().st_size <= 65536,
                "unsafe evidence file")
        require(hashlib.sha256(path.read_bytes()).hexdigest() == digest,
                "evidence checksum mismatch")
    require(observed == names, "manifest coverage mismatch")
    require((directory / "bootstrap-result.txt").read_bytes() == b"result=passed\n",
            "process test did not pass")
    events = json.loads((directory / "events.json").read_bytes())
    calibration = [event["calibration"] for event in events if "calibration" in event]
    require(len(calibration) == 1 and calibration[0]["ready"] and
            calibration[0]["accepted"] == 1, "missing calibrated observer")
    rejected = [event for event in events if "case" in event]
    require([event["case"] for event in rejected] ==
            ["network", "genesis", "magic", "late-genesis"], "missing rejection case")
    for event in rejected:
        require(event["exit"] == 2 and event["observer"]["ready"] and
                event["observer"]["accepted"] == 0 and
                0.2 <= event["observer"]["interval_seconds"] < 30 and
                event["persistence"] == "absent", "invalid rejection observation")
        require((directory / f"rejected-{event['case']}.txt").read_bytes() ==
                b"invalid static bootstrap configuration\n", "wrong diagnostic")
    sync = [event for event in events if "bootstrap_sync" in event]
    controls = [event for event in events if "restart_sync" in event]
    require(len(sync) == 1 and len(controls) == 1, "missing positive controls")
    require(sync[0]["bootstrap_sync"]["blocks"] == 2 and
            controls[0]["confirmed"]["blocks"] == 3 and
            controls[0]["restart_sync"]["blocks"] == 4 and
            controls[0]["restart_persistence"] is True, "incomplete state transitions")
    require(events[-1] == {"owned_children": "reaped", "listeners": "absent"},
            "missing shutdown proof")
    launches = [event["launch"] for event in events if "launch" in event]
    require(len(launches) == 7, "unexpected process topology")
    for args in launches:
        require("--regtest" in args and "--connect" not in args and
                "--enable-dns-seeds" not in args, "alternate bootstrap route")
    canonical = (directory / "canonical-regtest.conf").read_text(encoding="ascii")
    require(canonical.startswith(IDENTITY + "seed=127.0.0.1:"), "wrong canonical fixture")


class Harness:
    def __init__(self, daemon, runtime, evidence):
        self.daemon = str(Path(daemon).resolve(strict=True))
        self.runtime = runtime
        self.evidence = evidence
        self.children = []
        self.outputs = []
        self.events = []

    def start(self, name, p2p, rpc, bootstrap=None):
        root = self.runtime / name
        root.mkdir(exist_ok=True)
        data = root / "data"
        data.mkdir(exist_ok=True)
        args = [self.daemon, "--regtest", "--name", name, "--datadir", str(data),
                "--p2pbind", "127.0.0.1", "--p2pport", str(p2p), "--rpcport", str(rpc),
                "--logfile", str(root / "node.log")]
        if bootstrap is not None:
            args += ["--bootstrap", str(bootstrap)]
        require("--connect" not in args and "--enable-dns-seeds" not in args,
                "unexpected alternate bootstrap route")
        env = os.environ.copy()
        env.update(NOVACOIN_WALLET_PASSPHRASE="bootstrap-test-wallet-passphrase",
                   NOVACOIN_RPC_PASSWORD=PASSWORD)
        output = open(root / "output.log", "wb")
        self.outputs.append(output)
        child = subprocess.Popen(args, env=env, stdout=output, stderr=subprocess.STDOUT)
        self.children.append(child)
        self.events.append({"launch": args, "pid": child.pid})
        return child

    def check_logs(self):
        for path in self.runtime.rglob("*.log"):
            require(path.stat().st_size <= 1024 * 1024, "log bound exceeded")

    def rpc(self, port, method, params=None):
        self.check_logs()
        require(method != "connectpeer", "manual connection forbidden")
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=0.5)
        try:
            auth = base64.b64encode(f"novacoin:{PASSWORD}".encode()).decode()
            connection.request("POST", "/rpc", json.dumps({"jsonrpc": "2.0", "id": 1,
                               "method": method, "params": params or {}}),
                               {"Authorization": "Basic " + auth,
                                "Content-Type": "application/json"})
            response = connection.getresponse()
            raw = response.read(65537)
            require(len(raw) <= 65536 and response.status == 200, "invalid RPC response")
            result = json.loads(raw)
            require("error" not in result, f"RPC {method} rejected")
            return result["result"]
        finally:
            connection.close()

    def until(self, action, description, seconds=30):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                if action():
                    return
            except (OSError, http.client.HTTPException):
                pass
            time.sleep(0.05)
        raise RuntimeError("deadline: " + description)

    def state(self, port):
        info = self.rpc(port, "getnodehealth")
        return {"blocks": info["height"], "bestblockhash": info["bestblockhash"],
                "chainwork": info["chainwork"]}

    def stop_child(self, child):
        if child.poll() is None:
            child.terminate()
        try:
            code = child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait(timeout=2)
            raise RuntimeError("forced child termination")
        require(code == 0, f"daemon shutdown failed: {code}")

    def cleanup(self):
        forced = False
        for child in self.children:
            if child.poll() is None:
                child.terminate()
        for child in self.children:
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                forced = True
                child.kill()
                child.wait(timeout=2)
        for output in self.outputs:
            output.close()
        require(not forced, "cleanup required forced termination")
        require(all(child.poll() is not None for child in self.children), "child survived")

    def run(self):
        calibration = Sentinel()
        try:
            with socket.create_connection(("127.0.0.1", calibration.port), timeout=1):
                pass
        finally:
            observed = calibration.finish()
        require(observed["accepted"] == 1, "sentinel calibration failed")
        self.events.append({"calibration": observed})

        source_p2p, source_rpc, join_p2p, join_rpc = reserve_ports(4)
        source = self.start("source", source_p2p, source_rpc)
        self.until(lambda: self.state(source_rpc)["blocks"] == 0, "source startup", 10)
        self.rpc(source_rpc, "generateregtestblock")
        self.rpc(source_rpc, "generateregtestblock")
        baseline = self.state(source_rpc)
        baseline_pool = self.rpc(source_rpc, "getmempoolinfo")

        for case in ("network", "genesis", "magic", "late-genesis"):
            observer = Sentinel()
            try:
                identity = IDENTITY
                if case == "network":
                    identity = identity.replace("network=regtest", "network=testnet")
                elif case == "magic":
                    identity = identity.replace("magic=dab5bffa", "magic=dab5bff0")
                else:
                    identity = identity.replace("genesis=c", "genesis=d")
                seed = f"seed=127.0.0.1:{observer.port}\n"
                if case == "late-genesis":
                    prefix, genesis = identity.split("genesis=")
                    identity = prefix + seed + "genesis=" + genesis
                else:
                    identity += seed
                config = self.evidence / f"rejected-{case}.conf"
                write(config, identity)
                p2p, rpc = reserve_ports(2)
                child = self.start("reject-" + case, p2p, rpc, config)
                code = child.wait(timeout=10)
                self.check_logs()
                raw = (self.runtime / ("reject-" + case) / "output.log").read_bytes()
                require(code == 2, f"{case}: expected exit 2, got {code}")
                require(raw == b"invalid static bootstrap configuration\n",
                        f"{case}: wrong rejection diagnostic")
                write(self.evidence / f"rejected-{case}.txt", raw.decode())
                require(not any((self.runtime / ("reject-" + case) / "data").iterdir()),
                        "rejected configuration created persistence")
                for port in (p2p, rpc):
                    with socket.socket() as probe:
                        require(probe.connect_ex(("127.0.0.1", port)) != 0,
                                "rejected daemon left a listener")
            finally:
                observed = observer.finish()
            require(observed["accepted"] == 0, "rejected file initiated connection")
            require(self.state(source_rpc) == baseline, "rejection changed source chain")
            require(self.rpc(source_rpc, "getmempoolinfo") == baseline_pool,
                    "rejection changed source mempool")
            self.events.append({"case": case, "exit": code, "observer": observed,
                                "persistence": "absent", "source_state": baseline})

        config = self.evidence / "canonical-regtest.conf"
        write(config, IDENTITY + f"seed=127.0.0.1:{source_p2p}\n")
        joiner = self.start("joiner", join_p2p, join_rpc, config)
        self.until(lambda: self.state(join_rpc) == baseline, "bootstrap-only exact tip")
        require(self.rpc(join_rpc, "getpeerinfo")["count"] == 1, "unexpected joiner peers")
        require(self.rpc(source_rpc, "getpeerinfo")["count"] == 1, "unexpected source peers")
        self.events.append({"bootstrap_sync": self.state(join_rpc),
                            "handshake": "inferred_from_handshake_gated_block_sync"})
        address = self.rpc(join_rpc, "getnewaddress")
        txid = self.rpc(source_rpc, "sendtoaddress", {"address": address, "amount": 100000000})
        self.until(lambda: self.rpc(join_rpc, "getmempoolinfo")["size"] == 1,
                   "valid transaction propagation")
        self.rpc(source_rpc, "generateregtestblock")
        confirmed = self.state(source_rpc)
        self.until(lambda: self.state(join_rpc) == confirmed and
                   self.rpc(join_rpc, "getmempoolinfo")["size"] == 0,
                   "valid confirmation")
        self.stop_child(joiner)
        self.until(lambda: self.rpc(source_rpc, "getpeerinfo")["count"] == 0,
                   "old peer removal")
        joiner = self.start("joiner", join_p2p, join_rpc, config)
        self.until(lambda: self.state(join_rpc) == confirmed, "restart persistence", 10)
        self.rpc(source_rpc, "generateregtestblock")
        final = self.state(source_rpc)
        self.until(lambda: self.state(join_rpc) == final, "restart bootstrap catch-up")
        self.events.append({"valid_transaction": txid, "confirmed": confirmed,
                            "restart_persistence": True, "restart_sync": final})
        self.stop_child(joiner)
        self.stop_child(source)
        for port in (source_p2p, source_rpc, join_p2p, join_rpc):
            with socket.socket() as probe:
                require(probe.connect_ex(("127.0.0.1", port)) != 0,
                        "daemon listener survived shutdown")
        require(all(child.poll() is not None for child in self.children), "child survived")
        self.events.append({"owned_children": "reaped", "listeners": "absent"})


def main():
    require(len(sys.argv) == 3, "usage: test_static_bootstrap.py daemon evidence-root")
    if sys.argv[1] == "--verify":
        verify_evidence(Path(sys.argv[2]))
        print("verified static-bootstrap manifest and process observations")
        return
    evidence_root = Path(sys.argv[2]).resolve()
    evidence_root.mkdir(parents=True, exist_ok=True)
    evidence = Path(tempfile.mkdtemp(prefix="bootstrap.", dir=evidence_root))
    # Runtime wallet/data/logs never enter the uploaded evidence directory.
    with tempfile.TemporaryDirectory(prefix="nova-bootstrap-runtime.") as runtime:
        harness = Harness(sys.argv[1], Path(runtime), evidence)
        result = "failed"
        try:
            harness.run()
            result = "passed"
        except Exception as error:
            write(evidence / "failure.txt", f"{type(error).__name__}: {error}\n")
            raise
        finally:
            try:
                harness.cleanup()
            except Exception:
                result = "failed"
                raise
            finally:
                write(evidence / "events.json", json.dumps(harness.events, indent=2) + "\n")
                write(evidence / "bootstrap-result.txt", f"result={result}\n")
                lines = []
                for path in sorted(evidence.iterdir()):
                    lines.append(hashlib.sha256(path.read_bytes()).hexdigest() + "  " + path.name)
                write(evidence / "SHA256SUMS", "\n".join(lines) + "\n")
                for line in lines:
                    digest, name = line.split("  ")
                    require(hashlib.sha256((evidence / name).read_bytes()).hexdigest() == digest,
                            "evidence checksum mismatch")
                if result == "passed":
                    verify_evidence(evidence)
    print(f"static bootstrap evidence: {evidence}")


if __name__ == "__main__":
    def interrupted(number, _frame):
        raise RuntimeError(f"interrupted: {number}")

    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    main()
