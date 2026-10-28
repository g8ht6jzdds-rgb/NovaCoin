#!/usr/bin/env python3
"""Assertions for the existing four-process REGTEST harness; no production API."""

import base64
import hashlib
import http.client
import json
import os
from pathlib import Path
import re
import socket
import struct
import sys
import time


PORTS = (29643, 29642, 29641, 29640)
READS = {'getnodehealth', 'getpeerinfo', 'getmempoolinfo', 'gettransaction',
         'getutxo', 'listunspent', 'getbalances'}


def require(condition, description):
    if not condition:
        raise RuntimeError(description)


def remaining_seconds(deadline):
    left = deadline - time.monotonic()
    if left <= 0:
        raise TimeoutError('RPC timeout')
    return left


def chain(value):
    require(type(value['height']) is int and value['height'] >= 0, 'invalid height')
    require(re.fullmatch('[0-9a-f]{64}', value['bestblockhash']), 'invalid tip')
    require(re.fullmatch('[0-9a-f]{128}', value['chainwork']), 'invalid work')
    return {key: value[key] for key in ('height', 'bestblockhash', 'chainwork')}


def extends(after, before, height):
    require(chain(after)['height'] == height and
            after['bestblockhash'] != before['bestblockhash'] and
            int(after['chainwork'], 16) > int(before['chainwork'], 16), 'chain did not advance')


def transaction(raw, txid):
    """Bounded canonical fixture inspection, not a replacement validator."""
    require(isinstance(raw, str) and re.fullmatch('[0-9a-f]+', raw) and
            len(raw) <= 131072 and len(raw) % 2 == 0, 'invalid fixture hex')
    data = bytes.fromhex(raw)
    require(hashlib.sha256(hashlib.sha256(data).digest()).hexdigest() == txid, 'wrong txid')
    offset = 0

    def take(size):
        nonlocal offset
        require(0 <= size <= 65536 and offset + size <= len(data), 'truncated fixture')
        result = data[offset:offset + size]
        offset += size
        return result

    def count():
        prefix = take(1)[0]
        if prefix < 253:
            return prefix
        width = {253: 2, 254: 4, 255: 8}[prefix]
        value = int.from_bytes(take(width), 'little')
        require(value >= {253: 253, 254: 65536, 255: 4294967296}[prefix], 'noncanonical fixture')
        return value

    take(4)
    inputs = []
    size = count()
    require(0 < size <= 16, 'input fixture bound')
    for _ in range(size):
        inputs.append({'txid': take(32).hex(), 'vout': int.from_bytes(take(4), 'little')})
        take(count())
        take(4)
    outputs = []
    size = count()
    require(0 < size <= 16, 'output fixture bound')
    for index in range(size):
        amount = struct.unpack('<q', take(8))[0]
        require(amount > 0, 'fixture output amount')
        take(count())
        outputs.append({'txid': txid, 'vout': index, 'amount': amount})
    take(4)
    require(offset == len(data), 'fixture trailing bytes')
    return inputs, outputs


class Exercise:
    def __init__(self):
        self.events = []
        self.deadline = None
        self.checkpoints = {}

    def rpc(self, port, method, params=None, absent=False):
        require(port in PORTS, 'non-loopback harness port')
        require(self.deadline is None or method in READS, 'mutation inside retry poll')
        budget = 2 if method in READS else 10
        if self.deadline is not None:
            budget = min(budget, self.deadline - time.monotonic())
        require(budget > 0, 'RPC deadline exhausted')
        start = time.monotonic()
        deadline = start + budget
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=budget)
        outcome = 'failed'
        try:
            connection.connect()
            transport = connection.sock

            def remaining():
                transport.settimeout(remaining_seconds(deadline))

            auth = base64.b64encode(('novacoin:' + os.environ['NOVACOIN_RPC_PASSWORD']).encode()).decode()
            remaining()
            connection.request('POST', '/rpc', json.dumps({'jsonrpc': '2.0', 'id': 1,
                               'method': method, 'params': params or {}}),
                               {'Authorization': 'Basic ' + auth, 'Content-Type': 'application/json'})
            remaining()
            response = connection.getresponse()
            remaining()
            raw = response.read(65537)
            remaining_seconds(deadline)
            require(len(raw) <= 65536 and
                    response.status == 200, 'invalid/budget-exceeding RPC response')
            value = json.loads(raw)
            if absent:
                require(value.get('error', {}).get('message') == 'UTXO not found', 'expected absent UTXO')
                result = None
            else:
                require('error' not in value, 'RPC error: ' + method)
                result = value['result']
            outcome = 'passed'
            return result
        finally:
            connection.close()
            self.events.append({'port': port, 'method': method, 'outcome': outcome,
                                'elapsed_seconds': round(time.monotonic() - start, 6),
                                'budget_seconds': budget})

    def until(self, action, description, seconds=20):
        require(self.deadline is None, 'nested poll')
        self.deadline = time.monotonic() + seconds
        try:
            while time.monotonic() < self.deadline:
                try:
                    if action():
                        return
                except (OSError, TimeoutError):
                    pass  # read-only observation; mutations never retried
                time.sleep(min(0.05, max(0, self.deadline - time.monotonic())))
            raise RuntimeError('deadline: ' + description)
        finally:
            self.deadline = None

    def tip(self, port):
        return chain(self.rpc(port, 'getnodehealth'))

    def peers(self, port):
        value = self.rpc(port, 'getpeerinfo')['count']
        require(type(value) is int and value >= 0, 'invalid peer count')
        return value

    def snapshot(self, inputs, outputs):
        wallet = self.rpc(PORTS[0], 'listunspent')
        require(all(item['confirmed'] is True for item in wallet), 'unconfirmed wallet snapshot')
        balances = self.rpc(PORTS[0], 'getbalances')
        mempool = self.rpc(PORTS[0], 'getmempoolinfo')
        require(mempool == {'size': 0, 'bytes': 0}, 'nonempty confirmed snapshot')
        for item in inputs:
            self.rpc(PORTS[0], 'getutxo', item, absent=True)
        coins = [self.rpc(PORTS[0], 'getutxo', {'txid': item['txid'], 'vout': item['vout']})
                 for item in outputs]
        require(all(coin['amount'] == output['amount'] for coin, output in zip(coins, outputs)),
                'confirmed output value')
        return {'chain': self.tip(PORTS[0]), 'wallet': sorted(wallet, key=lambda c: (c['txid'], c['vout'])),
                'balances': balances, 'coins': coins, 'mempool': mempool}

    def fork(self):
        a, b, c, d = PORTS
        parent = self.tip(a)
        require(parent['height'] == 4, 'wrong common parent height')
        self.until(lambda: all(self.tip(port) == parent for port in PORTS), 'common exact parent')
        for port in PORTS:
            self.rpc(port, 'disconnectpeers')
        self.until(lambda: all(self.peers(port) == 0 for port in PORTS), 'observed isolation', 10)
        isolated_peers = {str(port): self.peers(port) for port in PORTS}
        isolated = {str(port): self.tip(port) for port in PORTS}
        self.rpc(c, 'connectpeer', {'host': '127.0.0.1', 'port': 29647})
        self.until(lambda: self.peers(c) == 1 and self.peers(d) == 1, 'winning pair')
        self.rpc(c, 'generateregtestblock')
        self.until(lambda: self.tip(d) == self.tip(c), 'first winning block')
        require(self.tip(a) == parent and self.tip(b) == parent and
                self.peers(a) == self.peers(b) == 0, 'partition leaked')
        address = self.rpc(b, 'getnewaddress')
        txid = self.rpc(a, 'sendtoaddress', {'address': address, 'amount': 100000000})
        raw = self.rpc(a, 'gettransaction', {'txid': txid})['hex']
        inputs, outputs = transaction(raw, txid)
        input_coins = [self.rpc(a, 'getutxo', item) for item in inputs]
        self.rpc(a, 'generateregtestblock')
        losing = self.tip(a)
        extends(losing, parent, 5)
        for item in inputs:
            self.rpc(a, 'getutxo', item, absent=True)
        losing_coins = [self.rpc(a, 'getutxo', {'txid': item['txid'], 'vout': item['vout']})
                        for item in outputs]
        self.rpc(c, 'generateregtestblock')
        winning = self.tip(c)
        self.until(lambda: self.tip(d) == winning, 'winning exact tip')
        extends(winning, losing, 6)
        require(self.tip(a) == losing and self.tip(b) == parent and
                self.peers(a) == self.peers(b) == 0, 'unexpected partition connection')
        self.rpc(a, 'connectpeer', {'host': '127.0.0.1', 'port': 29646})
        self.until(lambda: self.tip(a) == winning and self.rpc(a, 'getmempoolinfo')['size'] == 1,
                   'exact reorg and resurrection')
        selected = self.tip(a)
        resurrected = self.rpc(a, 'gettransaction', {'txid': txid})['hex']
        require(resurrected == raw, 'wrong resurrected transaction')
        restored = [self.rpc(a, 'getutxo', item) for item in inputs]
        require(restored == input_coins, 'inputs not restored by reorg')
        for item in outputs:
            self.rpc(a, 'getutxo', {'txid': item['txid'], 'vout': item['vout']}, absent=True)
        self.rpc(a, 'generateregtestblock')
        confirmed = self.tip(a)
        extends(confirmed, winning, 7)
        self.until(lambda: all(self.tip(port) == confirmed and
                              self.rpc(port, 'getmempoolinfo')['size'] == 0 for port in (a, c, d)),
                   'reconfirmed propagation')
        snapshot = self.snapshot(inputs, outputs)
        require(self.tip(b) == parent and self.peers(b) == 0, 'isolated Bravo changed')
        return {'parent': parent, 'isolated': isolated, 'isolated_peers': isolated_peers,
                'losing': losing, 'winning': winning, 'selected': selected,
                'txid': txid, 'raw': raw, 'resurrected': resurrected, 'inputs': inputs,
                'outputs': outputs, 'input_coins': input_coins, 'restored': restored,
                'losing_coins': losing_coins, 'confirmed': snapshot}

    def restart(self, record, second):
        a = PORTS[0]
        expected = record['after'] if second else record['confirmed']
        inputs, outputs = record['inputs'], record['outputs']
        self.until(lambda: self.tip(a) == expected['chain'], 'isolated exact restart', 10)
        require(self.peers(a) == 0, 'restart was not isolated')
        before = self.snapshot(inputs, outputs)
        require(before == expected, 'restart changed confirmed state')
        if second:
            # Charlie/Delta are live at the older winning tip, while Bravo has
            # remained isolated since the common parent. Reconnect both paths.
            self.checkpoints['before_reconnect'] = {str(port): self.tip(port) for port in PORTS}
            self.rpc(a, 'connectpeer', {'host': '127.0.0.1', 'port': 29646})
            self.rpc(PORTS[1], 'connectpeer', {'host': '127.0.0.1', 'port': 29644})

            def synchronized():
                tips = {str(port): self.tip(port) for port in PORTS}
                self.checkpoints['last_tips'] = tips
                return all(tip == before['chain'] for tip in tips.values())

            self.until(synchronized,
                       'post-restart exact propagation')
            require(all(self.rpc(port, 'getmempoolinfo') == {'size': 0, 'bytes': 0}
                        for port in PORTS), 'unexpected post-reconnect mempool')
            return {'before': before, 'isolated_peers': 0,
                    'catchup_nodes': {str(port): self.tip(port) for port in PORTS[:2]},
                    'all_nodes': {str(port): self.tip(port) for port in PORTS},
                    'before_reconnect': self.checkpoints['before_reconnect']}
        self.rpc(a, 'generateregtestblock')
        after = self.snapshot(inputs, outputs)
        extends(after['chain'], before['chain'], 8)
        return {'before': before, 'after': after, 'inputs': inputs, 'outputs': outputs,
                'isolated_peers': 0}


def verify(root):
    for name in ('first', 'second'):
        require((root / ('reorg-stop-' + name + '.txt')).read_bytes() == b'exit=0\n', 'unclean stop')
    require(json.loads((root / 'reorg-listeners.json').read_bytes()) == [True, True], 'listeners not closed')
    records = []
    for name in ('fork', 'restart-first', 'restart-second'):
        path = root / ('reorg-' + name + '.json')
        require(not path.is_symlink() and path.stat().st_size <= 262144, 'unsafe reorg evidence')
        value = json.loads(path.read_bytes())
        require(value['phase'] == name and value['result'] == 'passed' and value['events'], 'phase incomplete')
        require(all(e['port'] in PORTS and e['outcome'] in ('passed', 'failed') and
                    (e['outcome'] == 'passed' or e['method'] in READS) and
                    0 <= e['elapsed_seconds'] and 0 < e['budget_seconds'] <= 10 and
                    (e['outcome'] != 'passed' or e['elapsed_seconds'] <= e['budget_seconds'])
                    for e in value['events']),
                'RPC evidence failed')
        records.append(value['observations'])
    fork, first, second = records
    parent, losing, winning = (fork[key] for key in ('parent', 'losing', 'winning'))
    require(chain(parent)['height'] == 4 and
            fork['isolated'] == {str(port): parent for port in PORTS} and
            fork['isolated_peers'] == {str(port): 0 for port in PORTS}, 'common/isolation mismatch')
    extends(losing, parent, 5)
    extends(winning, losing, 6)
    require(fork['selected'] == winning, 'selected tip is not recorded winning tip')
    require(transaction(fork['raw'], fork['txid']) == (fork['inputs'], fork['outputs']) and
            fork['resurrected'] == fork['raw'] and fork['restored'] == fork['input_coins'], 'transaction mismatch')
    require(len(fork['input_coins']) == len(fork['inputs']) and
            len(fork['losing_coins']) == len(fork['outputs']), 'missing UTXO observations')
    for snapshot in (fork['confirmed'], first['before'], first['after'], second['before']):
        require(snapshot['mempool'] == {'size': 0, 'bytes': 0} and
                snapshot['balances']['unconfirmed'] == 0 and
                all(coin['confirmed'] is True and type(coin['amount']) is int and
                    coin['amount'] > 0 for coin in snapshot['wallet']) and
                snapshot['balances']['confirmed'] == sum(coin['amount'] for coin in snapshot['wallet']),
                'invalid confirmed wallet/mempool evidence')
        require(snapshot['coins'] == [{'amount': output['amount'], 'height': 7, 'coinbase': False}
                                      for output in fork['outputs']], 'confirmed UTXO mismatch')
    extends(fork['confirmed']['chain'], winning, 7)
    require(first['before'] == fork['confirmed'] and first['inputs'] == fork['inputs'] and
            first['outputs'] == fork['outputs'] and first['isolated_peers'] == 0,
            'first restart mismatch')
    extends(first['after']['chain'], first['before']['chain'], 8)
    require(second['before'] == first['after'] and second['isolated_peers'] == 0 and
            second['catchup_nodes'] == {str(port): second['before']['chain'] for port in PORTS[:2]},
            'second restart/propagation mismatch')
    require(second['all_nodes'] == {str(port): second['before']['chain'] for port in PORTS} and
            second['before_reconnect'] == {str(PORTS[0]): first['after']['chain'],
                                           str(PORTS[1]): parent,
                                           str(PORTS[2]): fork['confirmed']['chain'],
                                           str(PORTS[3]): fork['confirmed']['chain']},
            'four-node live reconnect proof missing')


def main():
    require(len(sys.argv) == 3, 'usage: test_reorg_persistence.py phase run-directory')
    phase, root = sys.argv[1], Path(sys.argv[2])
    if phase == '--closed':
        for port in (29644, 29643):
            with socket.socket() as probe:
                probe.settimeout(1)
                require(probe.connect_ex(('127.0.0.1', port)) != 0, 'surviving Alpha listener')
        path = root / 'reorg-listeners.json'
        observations = json.loads(path.read_bytes()) if path.exists() else []
        require(len(observations) < 2, 'extra stop observation')
        path.write_text(json.dumps(observations + [True]) + '\n', encoding='ascii')
        return
    if phase == '--verify':
        verify(root)
        print('verified exact reorg and two-restart evidence')
        return
    require(phase in ('fork', 'restart-first', 'restart-second'), 'unknown phase')
    exercise = Exercise()
    record = {'phase': phase, 'result': 'failed'}
    try:
        if phase == 'fork':
            observations = exercise.fork()
        else:
            previous = 'fork' if phase == 'restart-first' else 'restart-first'
            observations = exercise.restart(json.loads((root / ('reorg-' + previous + '.json')).read_bytes())['observations'],
                                            phase == 'restart-second')
        record.update(result='passed', observations=observations)
    finally:
        record['events'] = exercise.events
        record['checkpoints'] = exercise.checkpoints
        (root / ('reorg-' + phase + '.json')).write_bytes((json.dumps(record, indent=2) + '\n').encode())


if __name__ == '__main__':
    main()
