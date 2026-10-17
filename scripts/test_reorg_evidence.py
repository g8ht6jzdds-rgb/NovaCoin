#!/usr/bin/env python3
"""Offline negative controls for the retained reorg assertion verifier."""

import copy
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest

from test_reorg_persistence import PORTS, transaction, verify


class EvidenceTests(unittest.TestCase):
    def fixture(self, root):
        # Synthetic canonical bytes for verifier testing only, NOT a signed or
        # consensus-valid transaction fixture and never submitted to any node.
        raw = (struct.pack('<iB', 1, 1) + b'\x11' * 32 + struct.pack('<IBI', 0, 0, 0) +
               b'\x01' + struct.pack('<qB', 1, 0) + struct.pack('<I', 0)).hex()
        txid = hashlib.sha256(hashlib.sha256(bytes.fromhex(raw)).digest()).hexdigest()
        inputs, outputs = transaction(raw, txid)

        def tip(height):
            return {'height': height, 'bestblockhash': f'{height:064x}', 'chainwork': f'{height:0128x}'}

        def snapshot(height):
            return {'chain': tip(height), 'wallet': [], 'balances': {'confirmed': 0, 'unconfirmed': 0},
                    'coins': [{'amount': 1, 'height': 7, 'coinbase': False}],
                    'mempool': {'size': 0, 'bytes': 0}}

        fork = {'parent': tip(4), 'isolated': {str(p): tip(4) for p in PORTS},
                'isolated_peers': {str(p): 0 for p in PORTS}, 'losing': tip(5), 'winning': tip(6),
                'selected': tip(6),
                'txid': txid, 'raw': raw, 'resurrected': raw, 'inputs': inputs, 'outputs': outputs,
                'restored': [{'amount': 2}], 'input_coins': [{'amount': 2}],
                'losing_coins': [{'amount': 1}], 'confirmed': snapshot(7)}
        first = {'before': snapshot(7), 'after': snapshot(8), 'inputs': inputs,
                 'outputs': outputs, 'isolated_peers': 0}
        second = {'before': snapshot(8), 'isolated_peers': 0,
                  'catchup_nodes': {str(p): tip(8) for p in PORTS[:2]},
                  'all_nodes': {str(p): tip(8) for p in PORTS},
                  'before_reconnect': {str(PORTS[0]): tip(8), str(PORTS[1]): tip(4),
                                       str(PORTS[2]): tip(7), str(PORTS[3]): tip(7)}}
        for name, observations in zip(('fork', 'restart-first', 'restart-second'), (fork, first, second)):
            value = {'phase': name, 'result': 'passed', 'observations': observations,
                     'events': [{'port': PORTS[0], 'method': 'getnodehealth', 'outcome': 'passed',
                                 'elapsed_seconds': 0.01, 'budget_seconds': 2}]}
            (root / ('reorg-' + name + '.json')).write_text(json.dumps(value), encoding='ascii')
        for name in ('first', 'second'):
            (root / ('reorg-stop-' + name + '.txt')).write_bytes(b'exit=0\n')
        (root / 'reorg-listeners.json').write_text('[true,true]', encoding='ascii')

    def test_valid_and_corrupted_observations(self):
        with tempfile.TemporaryDirectory(prefix='nova-reorg-verifier.') as directory:
            root = Path(directory)
            self.fixture(root)
            verify(root)
            path = root / 'reorg-fork.json'
            original = json.loads(path.read_bytes())
            for key, value in [('bestblockhash', 'f' * 64), ('chainwork', '0' * 128)]:
                altered = copy.deepcopy(original)
                altered['observations']['winning'][key] = value
                path.write_text(json.dumps(altered), encoding='ascii')
                with self.subTest(key=key), self.assertRaises(RuntimeError):
                    verify(root)
            altered = copy.deepcopy(original)
            altered['observations']['txid'] = '0' * 64
            path.write_text(json.dumps(altered), encoding='ascii')
            with self.assertRaises(RuntimeError):
                verify(root)
            path.write_text(json.dumps(original), encoding='ascii')
            missing = root / 'reorg-restart-second.json'
            saved = missing.read_bytes()
            missing.unlink()
            with self.assertRaises(OSError):
                verify(root)
            missing.write_bytes(saved)
            value = json.loads(saved)
            value['observations']['all_nodes'][str(PORTS[2])]['height'] = 7
            missing.write_text(json.dumps(value), encoding='ascii')
            with self.assertRaises(RuntimeError):
                verify(root)
            missing.write_bytes(saved)
            (root / 'reorg-listeners.json').write_text('[true,false]', encoding='ascii')
            with self.assertRaises(RuntimeError):
                verify(root)
            (root / 'reorg-listeners.json').write_text('[true,true]', encoding='ascii')
            verify(root)

    def test_transaction_bounds(self):
        for raw in ('', '00', 'ff' * 65537):
            with self.subTest(length=len(raw)), self.assertRaises(RuntimeError):
                transaction(raw, '0' * 64)

    def test_canonical_parser_rejects_structured_invalid_fixtures(self):
        with tempfile.TemporaryDirectory(prefix='nova-reorg-parser.') as directory:
            root = Path(directory)
            self.fixture(root)
            record = json.loads((root / 'reorg-fork.json').read_bytes())['observations']
            raw = bytes.fromhex(record['raw'])
            # Correct hashes force execution of the parser rather than merely
            # exercising the txid check. Script/collection limits are test-local.
            cases = (raw[:-1], raw + b'\0', raw[:4] + b'\xfd\x01\x00' + raw[5:],
                     raw[:4] + b'\x11' + raw[5:], raw[:41] + b'\xff' * 9 + raw[42:])
            for data in cases:
                digest = hashlib.sha256(hashlib.sha256(data).digest()).hexdigest()
                with self.subTest(length=len(data)), self.assertRaises(RuntimeError):
                    transaction(data.hex(), digest)


if __name__ == '__main__':
    unittest.main()
