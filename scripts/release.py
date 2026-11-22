"""Fail-closed engineering release preparation. Never publishes or activates networks."""
import argparse
import fnmatch
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tarfile


CORE = r'(0|[1-9][0-9]*)'
SEMVER = re.compile(rf'{CORE}\.{CORE}\.{CORE}'
                    r'(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?'
                    r'(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?\Z')
MINIMUM_PATTERNS = ('src/consensus/*', 'src/primitives/*', 'src/crypto/*',
                    'src/chain/validation.*', 'src/chain/utxo.*',
                    'src/chain/chainstate.*', 'src/chain/network_chainstate.*',
                    'src/node/network_selection.*', 'docs/protocol.md',
                    'docs/chain-selection.md', 'docs/networks.md', '*CMakeLists.txt',
                    '*.cmake', 'CMakePresets.json', 'vcpkg*.json',
                    'contrib/release/*', 'scripts/release.py')
APPROVAL_FIELDS = ('protocol_documentation', 'new_consensus_tests', 'activation_design',
                   'backward_compatibility_analysis', 'testnet_testing', 'human_approval')


def version(text):
    match = SEMVER.fullmatch(text)
    if len(text) > 128 or not match or (match[4] and any(part.isdigit() and len(part) > 1 and part[0] == '0'
                                     for part in match[4].split('.'))):
        raise ValueError('invalid semantic version')
    return tuple(int(match[i]) for i in (1, 2, 3)), match[4]


def git_environment():
    return {key: value for key, value in os.environ.items() if not key.startswith('GIT_')}


def git(source, *args):
    result = subprocess.run(['git', '-C', str(source), *args], capture_output=True,
                            timeout=30, env=git_environment())
    if result.returncode:
        raise ValueError('Git inspection failed (no credentials or raw stderr printed)')
    return result.stdout


def identity(source):
    return git(source, 'rev-parse', 'HEAD').decode().strip()


def patterns(text):
    result = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        if len(line) > 200 or line.startswith('/') or '..' in line or '\\' in line:
            raise ValueError('unsafe critical-path designation')
        result.append(line)
    if not result or len(result) > 100:
        raise ValueError('empty or oversized critical-path designation')
    return result


def consensus_changes(source, base, head):
    # Pin object IDs; do not let later branch movement alter the comparison.
    base = git(source, 'rev-parse', '--verify', f'{base}^{{commit}}').decode().strip()
    head = git(source, 'rev-parse', '--verify', f'{head}^{{commit}}').decode().strip()
    git(source, 'merge-base', '--is-ancestor', base, head)
    selected = list(MINIMUM_PATTERNS)
    path = 'contrib/release/consensus-critical.txt'
    for commit in (base, head):
        present = git(source, 'ls-tree', '--name-only', commit, '--', path)
        if present:
            selected.extend(patterns(git(source, 'show', f'{commit}:{path}').decode('utf-8')))
    changed = git(source, 'diff', '--no-renames', '--name-only', '-z', base, head, '--')
    paths = sorted(p.decode('utf-8') for p in changed.split(b'\0') if p)
    critical = [p for p in paths if any(fnmatch.fnmatchcase(p, rule) for rule in selected)]
    return {'base': base, 'head': head, 'changed_paths': paths, 'critical_paths': critical,
            'requires_human_disposition': bool(critical)}


def require_clean(source, expected):
    if identity(source) != expected or git(source, 'status', '--porcelain', '--untracked-files=all'):
        raise ValueError('release preparation requires unchanged clean tracked/untracked inputs')


def run_gate(command, cwd, log, timeout=1800):
    # Only public build/test commands. No shell, environment dump, signing or secret inputs.
    with log.open('xb') as output:
        result = subprocess.run(command, cwd=cwd, stdout=output, stderr=subprocess.STDOUT,
                                timeout=timeout)
    if result.returncode:
        raise ValueError(f'required check failed; see {log.name}')


def check(source, build_root, evidence, profile, runner=run_gate):
    """Execute real checks; a supplied all-PASS JSON file cannot substitute for execution."""
    records = []

    def gate(name, command):
        runner(command, source, evidence / f'{name}.log')
        records.append({'check': name, 'command': command, 'exit_code': 0})

    def configure(label, extra):
        directory = build_root / label
        gate(f'{label}-configure', ['cmake', '-S', str(source), '-B', str(directory),
                                  '-G', 'Ninja', '-DNOVA_WARNINGS_AS_ERRORS=ON', *extra])
        gate(f'{label}-build', ['cmake', '--build', str(directory), '--parallel', '2'])
        return directory

    debug = configure('debug', ['-DCMAKE_BUILD_TYPE=Debug', '-DNOVA_ENABLE_CLANG_TIDY=ON'])
    gate('format', ['cmake', '-P', str(source / 'scripts/check-format.cmake')])
    for name, expression in (('unit', '^nova.unit$'), ('consensus', '^nova.consensus$'),
                             ('integration', '^nova.regtest_rpc_process$')):
        gate(name, ['ctest', '--test-dir', str(debug), '--no-tests=error',
                    '--output-on-failure', '-R', expression])
    gate('full-ctest', ['ctest', '--test-dir', str(debug), '--no-tests=error', '--output-on-failure'])
    if profile == 'rc':
        for name in ('network_integration', 'wallet_integration', 'reorg'):
            gate(name, ['ctest', '--test-dir', str(debug), '--no-tests=error',
                        '--output-on-failure', '-R', f'^nova.{name}$'])
        for sanitizer in ('ADDRESS', 'UNDEFINED_BEHAVIOR'):
            label = 'asan' if sanitizer == 'ADDRESS' else 'ubsan'
            directory = configure(label, ['-DCMAKE_BUILD_TYPE=RelWithDebInfo',
                                         '-DNOVA_ENABLE_SANITIZERS=ON',
                                         f'-DNOVA_ENABLE_{sanitizer}_SANITIZER=ON'])
            gate(f'{label}-ctest', ['ctest', '--test-dir', str(directory), '--no-tests=error',
                                   '--output-on-failure'])
    gate('metadata', ['cmake', '--build', str(debug), '--target', 'release-metadata'])
    return records, debug / 'release-metadata.json'


def disposition(report, record):
    if not report['critical_paths']:
        return 'NO_CONSENSUS_CHANGE'
    if record is None:
        raise ValueError('consensus-critical changes need an explicit hash-bound human disposition')
    if record.is_symlink() or record.stat().st_size > 1024 * 1024:
        raise ValueError('unsafe or oversized consensus disposition')
    data = json.loads(record.read_text(encoding='utf-8'))
    if not isinstance(data, dict):
        raise ValueError('consensus disposition must be an object')
    if any(data.get(key) != report[key] for key in ('base', 'head', 'critical_paths')):
        raise ValueError('consensus disposition does not bind the exact comparison')
    if (data.get('decision') != 'ACCEPTED' or not isinstance(data.get('reviewer'), str) or
            not data['reviewer'].strip()):
        raise ValueError('human consensus disposition is not accepted/attributable')
    effect = data.get('effect')
    if effect not in ('NO_CONSENSUS_CHANGE', 'CONSENSUS_CHANGE'):
        raise ValueError('human disposition must distinguish sensitive files from actual rule changes')
    if effect == 'NO_CONSENSUS_CHANGE' and (not isinstance(data.get('rationale'), str) or not data['rationale'].strip()):
        raise ValueError('a no-rule-change disposition needs an explicit reviewed rationale')
    fields = APPROVAL_FIELDS if effect == 'CONSENSUS_CHANGE' else ('human_approval',)
    for field in fields:
        reference = data.get(field, {})
        if not isinstance(reference, dict):
            raise ValueError('consensus evidence reference must be an object')
        path = reference.get('path', '')
        if (not isinstance(path, str) or not path or ':' in path or '\\' in path or PurePosixPath(path).is_absolute() or
                '..' in PurePosixPath(path).parts):
            raise ValueError('missing or unsafe consensus evidence reference')
        evidence = record.parent / path
        if (record.parent.resolve() not in evidence.resolve().parents or
                any(p.is_symlink() for p in [evidence, *evidence.parents]) or
                not evidence.is_file() or not 0 < evidence.stat().st_size <= 16 * 1024 * 1024):
            raise ValueError('consensus evidence is absent')
        if hashlib.sha256(evidence.read_bytes()).hexdigest() != reference.get('sha256'):
            raise ValueError('consensus evidence hash mismatch')
    # Verifies references and supplied decision binding, NOT human identity or substantive adequacy.
    return effect


def source_archive(source, commit, release_version, output):
    version(release_version)
    files = git(source, 'ls-tree', '-r', '-l', '-z', commit).split(b'\0')
    total = 0
    expected = {}
    if len(files) > 10001:
        raise ValueError('source archive exceeds the 10000-file bound')
    for raw in files:
        if not raw:
            continue
        attributes, path = raw.split(b'\t', 1)
        mode, kind, object_id, size = attributes.split()
        if mode not in (b'100644', b'100755') or kind != b'blob':
            raise ValueError('source archive requires ordinary tracked files, no links/submodules')
        total += int(size)
        expected[path.decode('utf-8')] = (mode, object_id.decode('ascii'), int(size))
        if total > 64 * 1024 * 1024:
            raise ValueError('source content exceeds the 64 MiB engineering archive bound')
        name = path.decode('utf-8').lower()
        if (name.endswith(('.pem', '.key', '.p12', '.pfx', '/.env')) or name == '.env' or
                '.gnupg/' in name):
            raise ValueError('sensitive filename in tracked release inputs')
    # Do not emit matching content, or ever write it into a release archive.
    secret = subprocess.run(['git', '-C', str(source), 'grep', '-I', '-q', '-E', '-e',
                             '-----BEGIN ([A-Z ]*PRIVATE KEY|PGP PRIVATE KEY BLOCK)-----',
                             commit, '--'], capture_output=True, timeout=30, env=git_environment())
    if secret.returncode != 1:
        raise ValueError('private-key material or unavailable secret scan blocks the source archive')
    with output.open('xb') as destination:
        subprocess.run(['git', '-C', str(source), '-c', 'core.autocrlf=false',
                        '-c', 'core.eol=lf', '-c', 'tar.umask=0022', 'archive', '--format=tar',
                        f'--prefix=NovaCoin-{release_version}/', commit], stdout=destination,
                       stderr=subprocess.DEVNULL, check=True, timeout=30, env=git_environment())
    # Export attributes can still omit/substitute/convert committed content.
    # Reject such output instead of normalizing it after hashing. Retain the
    # rejected TAR for diagnosis; preparation must not emit successful metadata.
    algorithm = git(source, 'rev-parse', '--show-object-format').decode().strip()
    if algorithm not in ('sha1', 'sha256'):
        raise ValueError('unsupported Git object format')
    prefix = f'NovaCoin-{release_version}/'
    seen = set()
    with tarfile.open(output, mode='r:') as archive:
        for member in archive:
            if member.isdir():
                continue
            if not member.isfile() or not member.name.startswith(prefix):
                raise ValueError('unexpected source archive member')
            name = member.name[len(prefix):]
            if name not in expected or name in seen:
                raise ValueError('source archive file set differs from commit')
            mode, object_id, size = expected[name]
            permissions = 0o755 if mode == b'100755' else 0o644
            if member.size != size or member.mode != permissions:
                raise ValueError('source archive size or permissions differ from canonical mode')
            stream = archive.extractfile(member)
            if stream is None:
                raise ValueError('source archive member unavailable')
            with stream:
                content = stream.read(size + 1)
            digest = hashlib.new(algorithm, f'blob {size}\0'.encode() + content).hexdigest()
            if len(content) != size or digest != object_id:
                raise ValueError('source archive bytes differ from committed blob')
            seen.add(name)
    if seen != set(expected):
        raise ValueError('source archive omits committed files')
    return hashlib.sha256(output.read_bytes()).hexdigest()


def manifest(directory, names):
    rows = []
    for name in sorted(names):
        path = directory / name
        if PurePosixPath(name).name != name or path.is_symlink() or not path.is_file():
            raise ValueError('unsafe manifest member')
        rows.append(f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {name}\n')
    with (directory / 'SHA256SUMS').open('w', encoding='utf-8', newline='\n') as output:
        output.write(''.join(rows))


def prepare(args):
    source = args.source.resolve()
    head = identity(source)
    require_clean(source, head)
    release_version = (source / 'VERSION').read_text(encoding='utf-8').strip()
    core, prerelease = version(release_version)
    if core[0] >= 1:
        raise ValueError('TASK023 does not authorize a 1.x/mainnet release')
    candidate = prerelease and any(re.fullmatch(r'rc[0-9]*', part, re.IGNORECASE) for part in prerelease.split('.'))
    profile = 'rc' if core[1] == 9 or candidate else args.profile
    output = args.output.resolve()
    if output == source or source in output.parents or output.exists():
        raise ValueError('use a new output directory outside the source tree')
    output.mkdir(parents=True)
    report = consensus_changes(source, args.base, head)
    (output / 'CONSENSUS-CHANGES.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    effect = disposition(report, args.consensus_decision)
    base_version = git(source, 'ls-tree', '--name-only', report['base'], '--', 'VERSION')
    previous = None
    if base_version:
        previous, _ = version(git(source, 'show', f"{report['base']}:VERSION").decode().strip())
    else:
        legacy = git(source, 'show', f"{report['base']}:CMakeLists.txt").decode('utf-8')
        declared = re.search(r'project\(NovaCoin\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)\s', legacy)
        if not declared:
            raise ValueError('previous baseline application version is unresolved')
        previous, _ = version(declared[1])
    if core[:2] == previous[:2] and core[2] > previous[2] and effect == 'CONSENSUS_CHANGE':
        raise ValueError('normal patch releases may not include consensus rule changes')
    records, metadata_path = check(source, args.build_root.resolve(), output, profile)
    metadata = json.loads(metadata_path.read_text(encoding='utf-8'))
    verify_metadata(metadata, release_version, head,
                    git(source, 'rev-parse', 'HEAD^{tree}').decode().strip())
    require_clean(source, head)
    shutil.copyfile(metadata_path, output / 'release-metadata.json')
    archive = f'NovaCoin-{release_version}-source.tar'
    source_archive(source, head, release_version, output / archive)
    require_clean(source, head)
    (output / 'CHECKS.json').write_text(json.dumps({'version': release_version, 'git_commit': head,
                                                  'profile': profile, 'checks': records}, indent=2) + '\n',
                                       encoding='utf-8')
    manifest(output, [archive, 'release-metadata.json', 'CHECKS.json', 'CONSENSUS-CHANGES.json'])
    print('Unsigned engineering preparation passed; human release/target/publication gates remain separate.')


def verify_metadata(data, release_version, head, tree):
    if (not isinstance(data, dict) or data.get('version') != release_version or
            data.get('git_commit') != head or data.get('git_tree') != tree or
            data.get('source_state') != 'clean'):
        raise ValueError('build metadata does not match the clean release input')
    for field in ('compiler', 'target_platform', 'configuration'):
        if not isinstance(data.get(field), str) or not re.fullmatch(r'[A-Za-z0-9_.+-]+', data[field]) or data[field] == 'UNRECORDED':
            raise ValueError('required public build metadata is missing')
    if 'build_timestamp' not in data or (data['build_timestamp'] is not None and
            (type(data['build_timestamp']) is not int or not 0 <= data['build_timestamp'] <= 253402300799)):
        raise ValueError('invalid reproducible build timestamp')
    networks = data.get('networks', {})
    if not isinstance(networks, dict) or set(networks) != {'regtest', 'testnet', 'mainnet'}:
        raise ValueError('network metadata inventory is incomplete')
    for network in networks.values():
        if (not isinstance(network, dict) or type(network.get('protocol_version')) is not int or
                type(network.get('minimum_peer_version')) is not int or
                not 0 < network['minimum_peer_version'] <= network['protocol_version']):
            raise ValueError('invalid network compatibility metadata')


def sign(args):
    fingerprint = args.fingerprint
    if not re.fullmatch(r'[0-9A-F]{40}|[0-9A-F]{64}', fingerprint):
        raise ValueError('supply an exact public signing fingerprint, not private-key material')
    path = args.manifest.resolve()
    if path.stat().st_size > 1024 * 1024:
        raise ValueError('oversized signing manifest')
    seen = set()
    for row in path.read_text(encoding='utf-8').splitlines():
        match = re.fullmatch(r'([a-f0-9]{64})  ([A-Za-z0-9_.+-]+)', row)
        if (not match or match[2] in ('.', '..') or match[2] in seen or
                (path.parent / match[2]).is_symlink()):
            raise ValueError('invalid signing manifest')
        seen.add(match[2])
        if hashlib.sha256((path.parent / match[2]).read_bytes()).hexdigest() != match[1]:
            raise ValueError('signing manifest hash mismatch')
    if not path.stat().st_size or args.output.exists():
        raise ValueError('empty manifest or existing signature output')
    # Custodian invocation only. No key import/export, passphrase argument, environment dump or CI use.
    if os.environ.get('CI'):
        raise ValueError('signing is restricted to the custodian environment, not CI')
    subprocess.run(['gpg', '--batch', '--local-user', fingerprint, '--armor', '--detach-sign',
                    '--output', str(args.output), str(path)], stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, check=True, timeout=60)
    result = subprocess.run(['gpg', '--batch', '--status-fd=1', '--verify', str(args.output), str(path)],
                            capture_output=True, timeout=30)
    valid = [line.split() for line in result.stdout.decode('ascii', errors='replace').splitlines()
             if line.startswith('[GNUPG:] VALIDSIG ')]
    if result.returncode or not any(parts[-1] == fingerprint for parts in valid):
        raise ValueError('detached signature verification/primary fingerprint mismatch')
    print('Detached manifest signature verified; no publication performed.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    compare = commands.add_parser('consensus-diff')
    compare.add_argument('--source', type=Path, default=Path('.'))
    compare.add_argument('--base', required=True)
    compare.add_argument('--head', default='HEAD')
    build = commands.add_parser('prepare')
    build.add_argument('--source', type=Path, default=Path('.'))
    build.add_argument('--base', required=True)
    build.add_argument('--build-root', type=Path, required=True)
    build.add_argument('--output', type=Path, required=True)
    build.add_argument('--profile', choices=('development', 'testnet', 'rc'), default='development')
    build.add_argument('--consensus-decision', type=Path)
    signing = commands.add_parser('sign')
    signing.add_argument('--manifest', type=Path, required=True)
    signing.add_argument('--output', type=Path, required=True)
    signing.add_argument('--fingerprint', required=True)
    args = parser.parse_args()
    try:
        if args.command == 'consensus-diff':
            print(json.dumps(consensus_changes(args.source, args.base, args.head), indent=2))
        elif args.command == 'prepare':
            prepare(args)
        else:
            sign(args)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        # Do not print command stderr, environment or arbitrary exception contents/credentials.
        print('Release operation refused: ' + (str(error) if isinstance(error, ValueError)
                                               else type(error).__name__), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
