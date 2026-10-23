"""Synthetic release-policy regression tests. No release keys or application services."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import run_test_group

import release


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='nova-release-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'source'
        self.source.mkdir()
        for args in (('init',), ('config', 'user.name', 'Synthetic Release Test'),
                     ('config', 'user.email', 'test@example.invalid')):
            release.git(self.source, *args)
        (self.source / 'VERSION').write_text('0.1.0-dev.1\n', encoding='utf-8')
        (self.source / 'README.md').write_text('Synthetic fixture only\n', encoding='utf-8')
        self.commit()
        self.base = release.identity(self.source)

    def commit(self):
        release.git(self.source, 'add', '.')
        release.git(self.source, '-c', 'commit.gpgsign=false', 'commit', '-m', 'synthetic fixture')

    def test_semver(self):
        for good in ('0.0.1', '0.7.0-testnet', '0.9.0-rc.1', '0.1.0-dev.1+build.01'):
            self.assertTrue(release.version(good))
        for bad in ('01.2.3', '0.1', '0.1.0-01', '0.1.0-rc..1', '0.1.0+', 'v0.1.0',
                    '0.1.0;bad', '0.1.0\n', '0.1.0-rc.01'):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                release.version(bad)

    def test_detects_deletions_and_old_designations(self):
        directory = self.source / 'contrib/release'
        directory.mkdir(parents=True)
        (directory / 'consensus-critical.txt').write_text('special.dat\n', encoding='utf-8')
        (self.source / 'special.dat').write_text('original\n', encoding='utf-8')
        self.commit()
        base = release.identity(self.source)
        (self.source / 'special.dat').unlink()
        (directory / 'consensus-critical.txt').write_text('other.dat\n', encoding='utf-8')
        self.commit()
        report = release.consensus_changes(self.source, base, 'HEAD')
        self.assertIn('special.dat', report['critical_paths'])
        self.assertIn('contrib/release/consensus-critical.txt', report['critical_paths'])
        with self.assertRaises(ValueError):
            release.disposition(report, None)

    def test_builtin_designations_and_no_false_document_hit(self):
        for path in ('src/consensus/amount.cpp', 'src/primitives/bytes.cpp',
                     'src/chain/validation.cpp', 'docs/protocol.md', 'notes.md'):
            target = self.source / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text('synthetic\n', encoding='utf-8')
        self.commit()
        report = release.consensus_changes(self.source, self.base, 'HEAD')
        self.assertEqual(len(report['critical_paths']), 4)
        self.assertNotIn('notes.md', report['critical_paths'])
        with self.assertRaises(ValueError):
            release.patterns('../unsafe\n')

    def test_every_required_failure_stops(self):
        failures = ('debug-configure', 'debug-build', 'format', 'unit', 'consensus',
                    'integration', 'full-ctest', 'network_integration', 'wallet_integration',
                    'reorg', 'asan-build', 'asan-ctest', 'ubsan-build', 'ubsan-ctest')
        for failure in failures:
            calls = []

            def runner(command, cwd, log):
                calls.append(log.stem)
                if log.stem == failure:
                    raise ValueError('synthetic injected failure')

            with self.subTest(failure=failure), self.assertRaises(ValueError):
                release.check(self.source, self.root / 'build', self.root, 'rc', runner)
            self.assertEqual(calls[-1], failure)

    def test_rc_matrix_and_static_enforcement(self):
        commands = []
        records, _ = release.check(self.source, self.root / 'build', self.root, 'rc',
                                   lambda cmd, cwd, log: commands.append(cmd))
        names = {item['check'] for item in records}
        self.assertTrue({'unit', 'consensus', 'integration', 'format', 'network_integration',
                         'wallet_integration', 'reorg', 'asan-ctest', 'ubsan-ctest'} <= names)
        self.assertIn('-DNOVA_ENABLE_CLANG_TIDY=ON', commands[0])
        self.assertIn('-DNOVA_WARNINGS_AS_ERRORS=ON', commands[0])
        self.assertTrue(all('--no-tests=error' in cmd for cmd in commands if cmd[0] == 'ctest'))

    def test_empty_named_group_fails_without_running(self):
        listed = subprocess.CompletedProcess(['synthetic'], 0, '', '')
        with patch.object(sys, 'argv', ['group', 'synthetic', '--gtest_filter=Missing.*']), \
                patch.object(subprocess, 'run', return_value=listed) as run:
            self.assertEqual(run_test_group.main(), 1)
        self.assertEqual(run.call_count, 1)

    def test_clean_input_and_release_candidate_cannot_downgrade(self):
        (self.source / 'VERSION').write_text('0.9.0-rc.1\n', encoding='utf-8')
        with self.assertRaises(ValueError):
            release.require_clean(self.source, self.base)
        self.commit()
        args = argparse.Namespace(source=self.source, base=self.base,
                                  output=self.root / 'release', build_root=self.root / 'build',
                                  profile='development', consensus_decision=None)
        with patch.object(release, 'check', side_effect=ValueError('stop before build')) as check:
            with self.assertRaises(ValueError):
                release.prepare(args)
            self.assertEqual(check.call_args.args[3], 'rc')
        for index, label in enumerate(('0.8.0-rc1', '0.8.0-RC.1')):
            (self.source / 'VERSION').write_text(label + '\n', encoding='utf-8')
            self.commit()
            args.output = self.root / f'release-{index}'
            with patch.object(release, 'check', side_effect=ValueError('stop before build')) as check:
                with self.assertRaises(ValueError):
                    release.prepare(args)
                self.assertEqual(check.call_args.args[3], 'rc')

    def test_mainnet_and_patch_consensus_release_refused(self):
        (self.source / 'VERSION').write_text('1.0.0\n', encoding='utf-8')
        self.commit()
        args = argparse.Namespace(source=self.source, base=self.base,
                                  output=self.root / 'release', build_root=self.root / 'build',
                                  profile='development', consensus_decision=None)
        with self.assertRaises(ValueError):
            release.prepare(args)
        (self.source / 'VERSION').write_text('0.1.1\n', encoding='utf-8')
        target = self.source / 'src/consensus/test.cpp'
        target.parent.mkdir(parents=True)
        target.write_text('synthetic\n', encoding='utf-8')
        self.commit()
        with patch.object(release, 'check') as check, self.assertRaises(ValueError):
            release.prepare(args)
        check.assert_not_called()

    def test_disposition_exact_hash_binding(self):
        report = {'base': self.base, 'head': self.base, 'critical_paths': ['docs/protocol.md']}
        path = self.root / 'decision.json'
        evidence = self.root / 'synthetic-evidence.txt'
        evidence.write_text('Synthetic fixture; not an actual human approval\n', encoding='utf-8')
        reference = {'path': evidence.name, 'sha256': hashlib.sha256(evidence.read_bytes()).hexdigest()}
        data = dict(report, decision='ACCEPTED', reviewer='synthetic test only', effect='CONSENSUS_CHANGE',
                    **{field: reference for field in release.APPROVAL_FIELDS})
        path.write_text(json.dumps(data), encoding='utf-8')
        release.disposition(report, path)
        evidence.write_text('changed\n', encoding='utf-8')
        with self.assertRaises(ValueError):
            release.disposition(report, path)

    def test_sensitive_file_no_rule_change_still_needs_human_evidence(self):
        report = {'base': self.base, 'head': self.base, 'critical_paths': ['CMakeLists.txt']}
        path = self.root / 'decision.json'
        approval = self.root / 'approval.txt'
        approval.write_text('Synthetic fixture only; no actual human decision\n', encoding='utf-8')
        data = dict(report, decision='ACCEPTED', reviewer='synthetic', effect='NO_CONSENSUS_CHANGE',
                    human_approval={'path': approval.name,
                                    'sha256': hashlib.sha256(approval.read_bytes()).hexdigest()})
        path.write_text(json.dumps(data), encoding='utf-8')
        with self.assertRaises(ValueError):
            release.disposition(report, path)
        data['rationale'] = 'Synthetic metadata-only fixture classification'
        path.write_text(json.dumps(data), encoding='utf-8')
        self.assertEqual(release.disposition(report, path), 'NO_CONSENSUS_CHANGE')
        data['effect'] = 'CONSENSUS_CHANGE'
        path.write_text(json.dumps(data), encoding='utf-8')
        with self.assertRaises(ValueError):
            release.disposition(report, path)

    def test_complete_metadata_and_successful_synthetic_preparation(self):
        (self.source / 'README.md').write_text('Second synthetic snapshot\n', encoding='utf-8')
        self.commit()
        head = release.identity(self.source)
        tree = release.git(self.source, 'rev-parse', 'HEAD^{tree}').decode().strip()
        metadata = {'version': '0.1.0-dev.1', 'git_commit': head, 'git_tree': tree,
                    'source_state': 'clean', 'compiler': 'Synthetic-1', 'target_platform': 'Fixture-x64',
                    'configuration': 'Debug', 'build_timestamp': None,
                    'networks': {name: {'protocol_version': 1, 'minimum_peer_version': 1}
                                 for name in ('regtest', 'testnet', 'mainnet')}}
        release.verify_metadata(metadata, '0.1.0-dev.1', head, tree)
        for field in ('version', 'git_commit', 'git_tree', 'compiler', 'target_platform',
                      'build_timestamp', 'networks'):
            invalid = dict(metadata)
            del invalid[field]
            with self.subTest(field=field), self.assertRaises(ValueError):
                release.verify_metadata(invalid, '0.1.0-dev.1', head, tree)
        path = self.root / 'synthetic-metadata.json'
        path.write_text(json.dumps(metadata), encoding='utf-8')
        args = argparse.Namespace(source=self.source, base=self.base, output=self.root / 'unsigned',
                                  build_root=self.root / 'build', profile='development', consensus_decision=None)
        with patch.object(release, 'check', return_value=([], path)):
            release.prepare(args)
        self.assertTrue((args.output / 'SHA256SUMS').is_file())
        self.assertTrue((args.output / 'NovaCoin-0.1.0-dev.1-source.tar').is_file())
        checks = json.loads((args.output / 'CHECKS.json').read_text(encoding='utf-8'))
        self.assertEqual(checks['git_commit'], head)

    def test_review_cannot_smuggle_actual_rule_change_into_patch(self):
        (self.source / 'VERSION').write_text('0.1.1\n', encoding='utf-8')
        target = self.source / 'src/consensus/synthetic.cpp'
        target.parent.mkdir(parents=True)
        target.write_text('synthetic\n', encoding='utf-8')
        self.commit()
        args = argparse.Namespace(source=self.source, base=self.base, output=self.root / 'unsigned',
                                  build_root=self.root / 'build', profile='development', consensus_decision=None)
        with patch.object(release, 'disposition', return_value='CONSENSUS_CHANGE'), \
                patch.object(release, 'check') as check:
            with self.assertRaises(ValueError):
                release.prepare(args)
            check.assert_not_called()

    def test_legacy_cmake_baseline_preserves_patch_rule(self):
        (self.source / 'VERSION').unlink()
        (self.source / 'CMakeLists.txt').write_text('project(NovaCoin VERSION 0.1.0 LANGUAGES CXX)\n', encoding='utf-8')
        self.commit()
        legacy_base = release.identity(self.source)
        (self.source / 'VERSION').write_text('0.1.1\n', encoding='utf-8')
        self.commit()
        args = argparse.Namespace(source=self.source, base=legacy_base, output=self.root / 'unsigned',
                                  build_root=self.root / 'build', profile='development', consensus_decision=None)
        with patch.object(release, 'disposition', return_value='CONSENSUS_CHANGE'), \
                patch.object(release, 'check') as check:
            with self.assertRaises(ValueError):
                release.prepare(args)
            check.assert_not_called()

    def test_source_archive_and_relative_checksums_are_repeatable(self):
        first, second = self.root / 'one.tar', self.root / 'two.tar'
        one = release.source_archive(self.source, self.base, '0.1.0-dev.1', first)
        two = release.source_archive(self.source, self.base, '0.1.0-dev.1', second)
        self.assertEqual(one, two)
        with tarfile.open(first) as archive:
            names = archive.getnames()
            self.assertTrue(all(name.startswith('NovaCoin-0.1.0-dev.1') for name in names))
            self.assertIn('NovaCoin-0.1.0-dev.1/VERSION', names)
            self.assertFalse(any('.git/' in name for name in names))
        release.manifest(self.root, ['one.tar', 'two.tar'])
        self.assertEqual((self.root / 'SHA256SUMS').read_bytes(),
                         f'{one}  one.tar\n{two}  two.tar\n'.encode())
        with self.assertRaises(FileExistsError):
            release.source_archive(self.source, self.base, '0.1.0-dev.1', first)

    def test_private_key_material_never_archived(self):
        marker = '-----BEGIN ' + 'PRIVATE KEY-----\nsynthetic, not a key\n'
        (self.source / 'unsafe.txt').write_text(marker, encoding='utf-8')
        self.commit()
        archive = self.root / 'unsafe.tar'
        with self.assertRaises(ValueError):
            release.source_archive(self.source, release.identity(self.source), '0.1.0', archive)
        self.assertFalse(archive.exists())

    def test_signing_never_runs_in_ci_or_for_modified_payload(self):
        payload = self.root / 'payload.txt'
        payload.write_text('synthetic\n', encoding='utf-8')
        release.manifest(self.root, ['payload.txt'])
        args = argparse.Namespace(manifest=self.root / 'SHA256SUMS',
                                  output=self.root / 'SHA256SUMS.asc', fingerprint='A' * 40)
        with patch.dict(os.environ, {'CI': 'true'}), patch.object(subprocess, 'run') as run:
            with self.assertRaises(ValueError):
                release.sign(args)
            run.assert_not_called()
        payload.write_text('changed\n', encoding='utf-8')
        with patch.object(subprocess, 'run') as run, self.assertRaises(ValueError):
            release.sign(args)
        run.assert_not_called()

    def test_signing_uses_public_selector_and_verifies_primary_fingerprint(self):
        payload = self.root / 'payload.txt'
        payload.write_text('synthetic\n', encoding='utf-8')
        release.manifest(self.root, ['payload.txt'])
        args = argparse.Namespace(manifest=self.root / 'SHA256SUMS',
                                  output=self.root / 'SHA256SUMS.asc', fingerprint='A' * 40)
        calls = []

        def fake(command, **kwargs):
            calls.append(command)
            status = f'[GNUPG:] VALIDSIG {"B" * 40} date 0 0 4 0 1 10 00 {args.fingerprint}\n'
            return subprocess.CompletedProcess(command, 0, status.encode(), b'')

        with patch.dict(os.environ, {'CI': ''}), patch.object(subprocess, 'run', side_effect=fake):
            release.sign(args)
        self.assertIn('--local-user', calls[0])
        self.assertIn('--verify', calls[1])
        self.assertNotIn('--passphrase', calls[0])


if __name__ == '__main__':
    unittest.main()
