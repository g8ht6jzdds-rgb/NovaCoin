#!/usr/bin/env python3
"""Real REGTEST discovery triangle and hostile-wire controls, loopback only."""

import hashlib
import json
from pathlib import Path
import signal
import select
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

from test_static_bootstrap import Harness, IDENTITY, Sentinel, require, reserve_ports, write


class IntroducerObserver:
    """One transparent A/B link, closed before the direct A/C control."""
    def __init__(self, target):
        self.target = target
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(1)
        self.listener.settimeout(0.1)
        self.port = self.listener.getsockname()[1]
        self.stop = threading.Event()
        self.error = None
        self.events = []
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        sockets = []
        deadline = time.monotonic() + 60
        try:
            while not self.stop.is_set() and time.monotonic() < deadline:
                try:
                    inbound, _ = self.listener.accept()
                    sockets.append(inbound)
                    break
                except socket.timeout:
                    pass
            require(sockets, 'introducer observer accept deadline')
            outbound = socket.create_connection(('127.0.0.1', self.target), timeout=1)
            sockets.append(outbound)
            for connection in sockets:
                connection.settimeout(0.5)
            buffers = [b'', b'']
            frames = 0
            while not self.stop.is_set() and time.monotonic() < deadline:
                ready, _, _ = select.select(sockets, [], [], 0.1)
                for connection in ready:
                    index = sockets.index(connection)
                    chunk = connection.recv(4096)
                    if not chunk:
                        return
                    sockets[1 - index].sendall(chunk)
                    buffers[index] += chunk
                    require(len(buffers[index]) <= 1024 * 1024 + 24, 'observer buffer bound')
                    while len(buffers[index]) >= 24:
                        size = struct.unpack('<I', buffers[index][16:20])[0]
                        require(size <= 1024 * 1024, 'observer frame bound')
                        if len(buffers[index]) < size + 24:
                            break
                        raw = buffers[index][:size + 24]
                        buffers[index] = buffers[index][size + 24:]
                        frames += 1
                        require(frames <= 1024, 'observer frame-count bound')
                        command = raw[4:16].rstrip(b'\0').decode('ascii')
                        if command in ('getaddr', 'addr'):
                            require(len(self.events) < 16, 'observer event bound')
                            self.events.append({'direction': 'a_to_b' if index == 0 else 'b_to_a',
                                                'command': command, 'frame_hex': raw.hex()})
            require(self.stop.is_set(), 'introducer observer deadline')
        except Exception as error:
            self.error = str(error)
        finally:
            for connection in sockets:
                connection.close()

    def finish(self):
        self.stop.set()
        self.thread.join(2)
        self.listener.close()
        require(not self.thread.is_alive() and self.error is None, f'observer: {self.error}')
        return self.events


def frame(command, payload=b''):
    checksum = hashlib.sha256(hashlib.sha256(payload).digest()).digest()[:4]
    return struct.pack('<I12sI', 0xdab5bffa, command.encode(), len(payload)) + checksum + payload


class WireSeed:
    def __init__(self, mode, advertised_port):
        self.mode = mode
        self.advertised_port = advertised_port
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(1)
        self.listener.settimeout(0.1)
        self.port = self.listener.getsockname()[1]
        self.stop = threading.Event()
        self.ready = threading.Event()
        self.sent = threading.Event()
        self.error = None
        self.commands = []
        self.responses = []
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()
        require(self.ready.wait(1), 'wire seed readiness')

    def run(self):
        self.ready.set()
        deadline = time.monotonic() + 15
        connection = None
        try:
            while not self.stop.is_set() and time.monotonic() < deadline:
                try:
                    connection, _ = self.listener.accept()
                    break
                except socket.timeout:
                    pass
            require(connection is not None, 'wire seed accept deadline')
            connection.settimeout(0.1)
            version = struct.pack('<iQqQ', 1, 0, 1000, 987654321) + b'\x05/test' + struct.pack('<iB', 0, 1)
            connection.sendall(frame('version', version) + frame('verack'))
            buffered = b''
            while not self.stop.is_set() and time.monotonic() < deadline:
                try:
                    part = connection.recv(4096)
                except socket.timeout:
                    continue
                if not part:
                    break
                buffered += part
                require(len(buffered) <= 65536, 'wire seed input bound')
                while len(buffered) >= 24:
                    magic, command, size = struct.unpack('<I12sI', buffered[:20])
                    require(magic == 0xdab5bffa and size <= 4096, 'wire seed frame bounds')
                    if len(buffered) < 24 + size:
                        break
                    payload = buffered[24:24 + size]
                    require(buffered[20:24] == hashlib.sha256(hashlib.sha256(payload).digest()).digest()[:4], 'wire checksum')
                    buffered = buffered[24 + size:]
                    command = command.rstrip(b'\0').decode('ascii')
                    require(len(self.commands) < 64, 'wire command bound')
                    self.commands.append(command)
                    if command == 'getaddr' and not self.sent.is_set():
                        address = struct.pack('<Q', 0) + b'\0' * 10 + b'\xff\xff' + bytes([127, 0, 0, 1]) + struct.pack('>H', self.advertised_port)
                        if self.mode == 'late':
                            # Local policy expires the solicited response at ten seconds.
                            require(not self.stop.wait(10.2), 'late control interrupted')
                        if self.mode == 'disallowed':
                            address = address[:23] + b'\x02' + address[24:]
                        payload = b'\x01' + address
                        if self.mode == 'malformed':
                            payload = b'\xfd\x01\x00' + address  # noncanonical CompactSize
                        if self.mode == 'unsolicited':
                            # First empty response consumes the request, then a valid
                            # candidate is unsolicited and must not create a dial.
                            encoded = frame('addr', b'\0') + frame('addr', payload)
                        else:
                            encoded = frame('addr', payload)
                        self.responses.append(encoded.hex())
                        connection.sendall(encoded)
                        self.sent.set()
                    elif command == 'getheaders':
                        connection.sendall(frame('headers', b'\0'))
            require(self.sent.is_set(), 'did not observe getaddr')
        except Exception as error:
            self.error = str(error)
        finally:
            if connection is not None:
                connection.close()

    def finish(self):
        self.stop.set()
        self.thread.join(2)
        self.listener.close()
        require(not self.thread.is_alive() and self.error is None, f'wire seed: {self.error}')
        return {'commands': self.commands, 'response_sent': self.sent.is_set(),
                'responses_hex': self.responses, 'seed_port': self.port,
                'advertised_port': self.advertised_port}


def triangle(harness, evidence, enabled):
    ports = reserve_ports(6)
    cp, cr, bp, br, ap, ar = ports
    c = harness.start('c', cp, cr, discovery=True)
    harness.until(lambda: harness.state(cr)['blocks'] == 0, 'C startup', 10)
    harness.rpc(cr, 'generateregtestblock')
    harness.rpc(cr, 'generateregtestblock')
    initial = harness.state(cr)
    bc = evidence / 'b-bootstrap.conf'
    ac = evidence / 'a-bootstrap.conf'
    write(bc, IDENTITY + f'seed=127.0.0.1:{cp}\n')
    write(ac, IDENTITY + f'seed=127.0.0.1:{bp}\n')
    b = harness.start('b', bp, br, bc, discovery=True)
    harness.until(lambda: harness.state(br) == initial, 'B exact tip')
    observer = IntroducerObserver(bp)
    # This sole bootstrap route is a transparent observer of B, never C.
    write(ac, IDENTITY + f'seed=127.0.0.1:{observer.port}\n')
    try:
        a = harness.start('a', ap, ar, ac, discovery=enabled)
        harness.until(lambda: harness.state(ar) == initial, 'A initial exact tip')
        expected = 2 if enabled else 1
        harness.until(lambda: harness.rpc(ar, 'getpeerinfo')['count'] == expected and
                      harness.rpc(cr, 'getpeerinfo')['count'] == expected, 'triangle topology')
        require(harness.rpc(br, 'getpeerinfo')['count'] == 2, 'unexpected introducer topology')
        harness.events.append({'topology': 'triangle' if enabled else 'line', 'initial': initial})
    finally:
        wire = observer.finish()
    if enabled:
        require(any(event['direction'] == 'a_to_b' and event['command'] == 'getaddr' for event in wire), 'missing getaddr observation')
        c_address = struct.pack('<Q', 0) + b'\0' * 10 + b'\xff\xff' + bytes([127, 0, 0, 1]) + struct.pack('>H', cp)
        require(any(event['direction'] == 'b_to_a' and event['command'] == 'addr' and
                    bytes.fromhex(event['frame_hex'])[24:] == b'\x01' + c_address for event in wire), 'missing actual C advertisement')
    else:
        require(not wire, 'disabled discovery emitted address traffic')
    harness.events.append({'introducer_wire': wire, 'observer_closed': True,
                           'introducer_port': bp, 'observer_port': observer.port, 'c_port': cp})
    harness.stop_child(b)
    expected = 1 if enabled else 0
    harness.until(lambda: harness.rpc(ar, 'getpeerinfo')['count'] == expected and
                  harness.rpc(cr, 'getpeerinfo')['count'] == expected, 'introducer removal')
    harness.rpc(cr, 'generateregtestblock')
    after = harness.state(cr)
    if enabled:
        harness.until(lambda: harness.state(ar) == after, 'direct C to A synchronization')
        address = harness.rpc(ar, 'getnewaddress')
        harness.rpc(cr, 'sendtoaddress', {'address': address, 'amount': 100000000})
        harness.until(lambda: harness.rpc(ar, 'getmempoolinfo')['size'] == 1, 'funding propagation')
        harness.rpc(cr, 'generateregtestblock')
        confirmed = harness.state(cr)
        harness.until(lambda: harness.state(ar) == confirmed, 'funding confirmation')
        recipient = harness.rpc(cr, 'getnewaddress')
        txid = harness.rpc(ar, 'sendtoaddress', {'address': recipient, 'amount': 50000000})
        harness.until(lambda: harness.rpc(cr, 'getmempoolinfo')['size'] == 1, 'direct valid transfer')
        harness.rpc(cr, 'generateregtestblock')
        final = harness.state(cr)
        harness.until(lambda: harness.state(ar) == final and harness.rpc(ar, 'getmempoolinfo')['size'] == 0,
                      'direct valid confirmation')
        harness.events.append({'introducer_stopped': True, 'direct_sync': after,
                               'final': final, 'valid_transaction': txid})
    else:
        for _ in range(10):
            require(harness.state(ar) == initial, 'discovery-disabled node unexpectedly synchronized')
            require(harness.rpc(ar, 'getpeerinfo')['count'] == 0, 'unexpected disabled connection')
            time.sleep(0.1)
        harness.events.append({'disabled_control': 'isolated_after_introducer_stop',
                               'held': initial, 'source': after})
    harness.stop_child(a)
    harness.stop_child(c)
    for port in ports:
        with socket.socket() as probe:
            require(probe.connect_ex(('127.0.0.1', port)) != 0, 'surviving daemon listener')
    harness.events.append({'owned_children': 'reaped', 'listeners': 'absent'})


def hostile(harness, evidence, mode):
    host = '127.0.0.2' if mode == 'disallowed' else '127.0.0.1'
    calibration = Sentinel(host)
    try:
        with socket.create_connection((host, calibration.port), timeout=1):
            pass
    finally:
        measured = calibration.finish()
    require(measured['accepted'] == 1, 'hostile sentinel calibration failed')
    harness.events.append({'sentinel_calibration': measured})
    sentinel = Sentinel(host)
    seed = WireSeed(mode, sentinel.port)
    try:
        pp, rp = reserve_ports(2)
        config = evidence / 'bootstrap.conf'
        write(config, IDENTITY + f'seed=127.0.0.1:{seed.port}\n')
        node = harness.start(mode, pp, rp, config, discovery=True)
        harness.until(lambda: harness.state(rp)['blocks'] == 0, 'hostile case startup', 10)
        before = harness.state(rp)
        require(seed.sent.wait(12), 'wire response deadline')
        for _ in range(10):
            require(harness.state(rp) == before, 'hostile gossip changed chain state')
            require(harness.rpc(rp, 'getmempoolinfo')['size'] == 0, 'hostile gossip changed mempool')
            time.sleep(0.1)
        harness.stop_child(node)
    finally:
        try:
            observation = sentinel.finish()
        finally:
            wire = seed.finish()
    require(observation['accepted'] == 0, 'hostile advertisement caused unintended connection')
    harness.events.append({'hostile_case': mode, 'sentinel': observation, 'wire': wire,
                           'sentinel_endpoint': f'{host}:{sentinel.port}', 'state': before})


def gates(harness):
    observations = []
    for network in ('testnet', 'mainnet', 'regtest'):
        pp, rp = reserve_ports(2)
        data = harness.runtime / network
        data.mkdir()
        args = [harness.daemon, '--' + network, '--peer-discovery', '--name', 'gate',
                '--datadir', str(data), '--p2pport', str(pp), '--rpcport', str(rp),
                '--logfile', str(harness.runtime / (network + '.log'))]
        if network == 'regtest':
            args += ['--p2pbind', '::1']
        result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=10, check=False)
        expected = 2 if network == 'regtest' else 1
        require(result.returncode == expected, 'activation/loopback gate exit')
        diagnostic = (b'peer discovery requires REGTEST IPv4 loopback\n' if network == 'regtest'
                      else (network + ' is disabled pending immutable parameter approval\n').encode())
        require(result.stdout == diagnostic and not list(data.iterdir()), 'gate diagnostic/persistence')
        for port in (pp, rp):
            with socket.socket() as probe:
                require(probe.connect_ex(('127.0.0.1', port)) != 0, 'gate left listener')
        observations.append({'network': network, 'exit': result.returncode,
                             'diagnostic': diagnostic.decode().strip(), 'persistence': 'absent'})
    harness.events.append({'disabled_network_gates': observations})


def verify(root):
    results = []
    cases = ('enabled', 'disabled', 'disallowed', 'malformed', 'late', 'unsolicited', 'gates')
    require({path.name for path in root.iterdir()} == set(cases), 'unexpected run files')
    for case in cases:
        directory = root / case
        names = {'events.json', 'discovery-result.txt'}
        if case in ('enabled', 'disabled'):
            names |= {'a-bootstrap.conf', 'b-bootstrap.conf'}
        elif case != 'gates':
            names.add('bootstrap.conf')
        require({path.name for path in directory.iterdir()} == names | {'SHA256SUMS'}, 'unexpected case files')
        require((directory / 'discovery-result.txt').read_bytes() == b'result=passed\n', 'case did not pass')
        manifest = (directory / 'SHA256SUMS').read_bytes()
        require(b'\r' not in manifest and manifest.endswith(b'\n'), 'manifest not LF-only')
        observed = set()
        lines = manifest.decode().splitlines()
        require(lines == sorted(lines, key=lambda line: line.split('  ')[1]), 'manifest order')
        for line in lines:
            digest, name = line.split('  ')
            require(name == Path(name).name and name not in observed and name != 'SHA256SUMS', 'unsafe entry')
            observed.add(name)
            require(name in names and not (directory / name).is_symlink() and
                    (directory / name).stat().st_size <= 65536, 'unsafe evidence file')
            require(hashlib.sha256((directory / name).read_bytes()).hexdigest() == digest, 'checksum mismatch')
        require(observed == {path.name for path in directory.iterdir()} - {'SHA256SUMS'}, 'manifest coverage')
        events = json.loads((directory / 'events.json').read_bytes())
        require(events[-1] == {'cleanup': 'passed'}, 'cleanup observation missing')
        for event in events:
            if 'launch' in event:
                require('--connect' not in event['launch'] and '--enable-dns-seeds' not in event['launch'], 'alternate manual route')
        if case in ('enabled', 'disabled'):
            observation = next(event for event in events if 'introducer_wire' in event)
            require(observation['observer_closed'], 'observer still available during direct control')
            require((directory / 'a-bootstrap.conf').read_bytes() ==
                    (IDENTITY + f"seed=127.0.0.1:{observation['observer_port']}\n").encode(), 'wrong A bootstrap')
            require((directory / 'b-bootstrap.conf').read_bytes() ==
                    (IDENTITY + f"seed=127.0.0.1:{observation['c_port']}\n").encode(), 'wrong B bootstrap')
            wire = observation['introducer_wire']
            if case == 'enabled':
                require(any(event['command'] == 'getaddr' and event['direction'] == 'a_to_b' for event in wire), 'getaddr missing')
                address = struct.pack('<Q', 0) + b'\0' * 10 + b'\xff\xff' + bytes([127, 0, 0, 1]) + struct.pack('>H', observation['c_port'])
                require(any(event['command'] == 'addr' and event['direction'] == 'b_to_a' and
                            bytes.fromhex(event['frame_hex']) == frame('addr', b'\x01' + address) for event in wire), 'C advertisement missing')
            else:
                require(not wire, 'disabled wire traffic')
        if case == 'enabled':
            require(any(event.get('introducer_stopped') and event['final']['blocks'] == 5 for event in events), 'direct proof missing')
        elif case == 'disabled':
            require(any('disabled_control' in event for event in events), 'disabled proof missing')
        elif case == 'gates':
            require(any(len(event.get('disabled_network_gates', [])) == 3 for event in events), 'gates missing')
        else:
            require(any(event.get('sentinel_calibration', {}).get('accepted') == 1 for event in events), 'sentinel calibration missing')
            require(any(event.get('hostile_case') == case and event['sentinel']['ready'] and
                        event['sentinel']['accepted'] == 0 and event['wire']['response_sent'] for event in events), 'hostile proof missing')
        results.append(case)
    return results


def main():
    require(len(sys.argv) == 3, 'usage: test_peer_discovery.py daemon evidence-root | --verify run-directory')
    if sys.argv[1] == '--verify':
        print('verified discovery evidence:', ','.join(verify(Path(sys.argv[2]))))
        return
    root = Path(sys.argv[2]).resolve()
    root.mkdir(parents=True, exist_ok=True)
    evidence = Path(tempfile.mkdtemp(prefix='discovery.', dir=root))
    for case in ('enabled', 'disabled', 'disallowed', 'malformed', 'late', 'unsolicited', 'gates'):
        directory = evidence / case
        directory.mkdir()
        with tempfile.TemporaryDirectory(prefix='nova-discovery-runtime.') as runtime:
            harness = Harness(sys.argv[1], Path(runtime), directory)
            result = 'failed'
            try:
                if case in ('enabled', 'disabled'):
                    triangle(harness, directory, case == 'enabled')
                elif case == 'gates':
                    gates(harness)
                else:
                    hostile(harness, directory, case)
                result = 'passed'
            except Exception as error:
                write(directory / 'failure.txt', f'{type(error).__name__}: {error}\n')
                raise
            finally:
                try:
                    harness.cleanup()
                    harness.events.append({'cleanup': 'passed'})
                except Exception:
                    result = 'failed'
                    raise
                finally:
                    write(directory / 'events.json', json.dumps(harness.events, indent=2) + '\n')
                    write(directory / 'discovery-result.txt', f'result={result}\n')
                    lines = [hashlib.sha256(path.read_bytes()).hexdigest() + '  ' + path.name
                             for path in sorted(directory.iterdir())]
                    write(directory / 'SHA256SUMS', '\n'.join(lines) + '\n')
    print('discovery evidence:', evidence, verify(evidence))


if __name__ == '__main__':
    def interrupted(number, _frame):
        raise RuntimeError(f'interrupted: {number}')

    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    main()
