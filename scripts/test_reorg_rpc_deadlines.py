"""Deterministic RPC deadline handling; no sockets, nodes, or credentials used."""
import os
import unittest
from unittest.mock import Mock, patch

import test_reorg_persistence as harness


class RpcDeadlines(unittest.TestCase):
    def test_positive_and_exact_expiry_boundary(self):
        with patch.object(harness.time, 'monotonic', return_value=4):
            self.assertEqual(harness.remaining_seconds(5), 1)
            for deadline in (4, 3):
                with self.subTest(deadline=deadline), self.assertRaises(TimeoutError):
                    harness.remaining_seconds(deadline)

    def exercise_rpc(self, expire_at=None):
        clock = [0.0]
        response = Mock(status=200)
        response.read.return_value = b'{"result":{"height":1}}'
        connection = Mock()
        connection.getresponse.return_value = response
        if expire_at == 'headers':
            def headers():
                clock[0] = 2.01
                return response
            connection.getresponse.side_effect = headers
        if expire_at == 'body':
            def body(_limit):
                clock[0] = 2.01
                return b'{"result":{"height":1}}'
            response.read.side_effect = body
        exercise = harness.Exercise()
        with patch.object(harness.time, 'monotonic', side_effect=lambda: clock[0]), \
                patch.object(harness.http.client, 'HTTPConnection', return_value=connection), \
                patch.dict(os.environ, {'NOVACOIN_RPC_PASSWORD': 'disposable-canary'}):
            if expire_at is None:
                self.assertEqual(exercise.rpc(harness.PORTS[0], 'getnodehealth'), {'height': 1})
                self.assertEqual(exercise.events[-1]['outcome'], 'passed')
            else:
                with self.assertRaises(TimeoutError):
                    exercise.rpc(harness.PORTS[0], 'getnodehealth')
                self.assertEqual(exercise.events[-1]['outcome'], 'failed')
                self.assertGreater(exercise.events[-1]['elapsed_seconds'], 2)
            connection.close.assert_called_once()
            connection.request.assert_called_once()

    def test_valid_read_and_expired_headers_and_body(self):
        for stage in (None, 'headers', 'body'):
            with self.subTest(stage=stage):
                self.exercise_rpc(stage)

    def test_read_timeout_retries_without_moving_outer_deadline(self):
        exercise = harness.Exercise()
        deadlines = []

        def observation():
            deadlines.append(exercise.deadline)
            if len(deadlines) == 1:
                raise TimeoutError('synthetic per-request expiry')
            return True

        with patch.object(harness.time, 'monotonic', return_value=0), \
                patch.object(harness.time, 'sleep'):
            exercise.until(observation, 'read-only observation', seconds=10)
        self.assertEqual(deadlines, [10, 10])
        self.assertIsNone(exercise.deadline)

    def test_outer_deadline_still_fails(self):
        exercise = harness.Exercise()
        action = Mock(side_effect=TimeoutError('synthetic timeout'))
        with patch.object(harness.time, 'monotonic', side_effect=[0, 0, 0, 1]), \
                patch.object(harness.time, 'sleep'), \
                self.assertRaisesRegex(RuntimeError, 'deadline: observation'):
            exercise.until(action, 'observation', seconds=1)
        action.assert_called_once()
        self.assertIsNone(exercise.deadline)

    def test_mutations_and_assertion_failures_are_not_retried(self):
        exercise = harness.Exercise()
        exercise.deadline = 20
        with patch.object(harness.http.client, 'HTTPConnection') as connection, \
                self.assertRaisesRegex(RuntimeError, 'mutation inside retry poll'):
            exercise.rpc(harness.PORTS[0], 'connectpeer')
        connection.assert_not_called()
        exercise.deadline = None
        action = Mock(side_effect=RuntimeError('state mismatch'))
        with self.assertRaisesRegex(RuntimeError, 'state mismatch'):
            exercise.until(action, 'must not mask assertions')
        action.assert_called_once()
        self.assertIsNone(exercise.deadline)


if __name__ == '__main__':
    unittest.main()
