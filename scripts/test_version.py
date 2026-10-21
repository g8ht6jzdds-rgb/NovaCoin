"""Bounded synthetic provenance fixtures and real --version subprocess tests."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile


def run(command, **kwargs):
    # Synthetic repositories must not inherit an outer worktree/index selection.
    kwargs.setdefault('env', {k: v for k, v in os.environ.items()
                              if not k.startswith('GIT_')})
    return subprocess.run(command, capture_output=True, timeout=10, **kwargs)


def inventory(root):
    return sorted((str(p.relative_to(root)),
                   hashlib.sha256(p.read_bytes()).hexdigest() if p.is_file() else 'directory')
                  for p in root.rglob('*'))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cmake', required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--header', type=Path, required=True)
    parser.add_argument('executables', nargs=5, type=Path)
    args = parser.parse_args()
    fields = re.findall(r'= "([^"]+)";', args.header.read_text())
    assert len(fields) == 4, fields
    version, revision, tree, state = fields
    assert re.fullmatch(r'\d+\.\d+\.\d+', version)
    assert state in ('clean', 'dirty', 'unknown', 'archive-declared')
    with tempfile.TemporaryDirectory(prefix='nova-version-') as directory:
        root = Path(directory)
        for index, timezone in enumerate(('UTC0', 'UTC+8')):
            cwd = root / str(index)
            cwd.mkdir()
            marker = cwd / 'untouched.txt'
            marker.write_bytes(b'synthetic fixture\n')
            before = inventory(cwd)
            env = dict(os.environ, TZ=timezone)
            env = {k: v for k, v in env.items() if not k.startswith('NOVACOIN_')}
            for executable in args.executables:
                result = run([str(executable), '--version'], cwd=cwd, env=env)
                name = executable.stem if os.name == 'nt' else executable.name
                expected = f'{name} {version} revision={revision} tree={tree} state={state}\n'
                assert result.returncode == 0 and not result.stderr, result
                assert result.stdout.decode().replace('\r\n', '\n') == expected, result.stdout
                # These combinations are rejected by parsers, never start services.
                for invalid in (['--version', '--version'], ['--version-extra']):
                    result = run([str(executable), *invalid], cwd=cwd, env=env)
                    assert result.returncode == (0 if name == 'novacoin-cli' else 2), result
                    assert not result.stdout, result.stdout
                assert inventory(cwd) == before

        repo = root / 'repo'
        repo.mkdir()
        output = root / 'output.hpp'

        def generate(extra=(), success=True):
            result = run([args.cmake, f'-DSOURCE_DIR={repo}', f'-DOUTPUT={output}',
                          '-DAPP_VERSION=0.1.0', *extra, '-P',
                          str(args.source / 'scripts/write-version.cmake')])
            assert (result.returncode == 0) == success, result.stderr
            return output.read_text() if success else ''

        assert '"unknown"' in generate()
        declared = ['-DARCHIVE_REVISION=' + 'a' * 40, '-DARCHIVE_TREE=' + 'b' * 40]
        assert '"archive-declared"' in generate(declared)
        generate(declared[:1], success=False)
        generate(['-DARCHIVE_REVISION=bad', declared[1]], success=False)
        generate(['-DAPP_VERSION=0.1.0;invalid'], success=False)
        (repo / '.git').write_text('gitdir: missing-synthetic-metadata\n')
        generate(success=False)
        (repo / '.git').unlink()
        for command in (['init'], ['config', 'user.name', 'Synthetic Test'],
                        ['config', 'user.email', 'test@example.invalid']):
            assert run(['git', '-C', str(repo), *command]).returncode == 0
        (repo / 'tracked').write_text('one\n')
        assert run(['git', '-C', str(repo), 'add', 'tracked']).returncode == 0
        assert run(['git', '-C', str(repo), '-c', 'commit.gpgsign=false',
                    'commit', '-m', 'synthetic']).returncode == 0
        clean = generate()
        assert '"clean"' in clean
        old_mtime = output.stat().st_mtime_ns
        assert generate() == clean and output.stat().st_mtime_ns == old_mtime
        (repo / 'tracked').write_text('two\n')
        assert '"dirty"' in generate()
        assert run(['git', '-C', str(repo), 'add', 'tracked']).returncode == 0
        assert '"dirty"' in generate()
        assert run(['git', '-C', str(repo), '-c', 'commit.gpgsign=false',
                    'commit', '-m', 'synthetic second']).returncode == 0
        assert generate() != clean and '"clean"' in generate()
        (repo / 'untracked').write_text('not committed\n')
        assert '"dirty"' in generate()
        generate(declared, success=False)
    print('five executable outputs, parser controls, immutability and provenance fixtures PASS')


if __name__ == '__main__':
    main()
