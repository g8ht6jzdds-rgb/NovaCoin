"""Check the offline corpus CLI's exact stdout discipline and file-side-effect boundary."""

import pathlib
import subprocess
import sys
import tempfile


def run(executable, *args, cwd):
    return subprocess.run([executable, *args], cwd=cwd, capture_output=True,
                          text=True, timeout=15, check=False)


def main():
    executable = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='nova-freeze-corpus-') as temporary:
        directory = pathlib.Path(temporary)
        before = sorted(path.name for path in directory.iterdir())
        version = run(executable, '--version', cwd=directory)
        if version.returncode != 0 or not version.stdout.startswith('nova-freeze-corpus '):
            print('standalone version request failed', file=sys.stderr)
            return 1
        result = run(executable, '--regtest-testnet', cwd=directory)
        # A dirty developer tree is intentionally fail-closed. A clean/archive build emits
        # canonical bytes; in either case no working-directory writes are permitted.
        if result.returncode not in (0, 1) or result.stderr and result.returncode == 0:
            print('corpus command had an unexpected result', file=sys.stderr)
            return 1
        if result.returncode == 0 and not result.stdout.startswith(
                'format=novacoin-gate1-parameter-corpus-v1\n'):
            print('corpus output was not canonical', file=sys.stderr)
            return 1
        invalid = run(executable, '--unexpected', cwd=directory)
        if invalid.returncode != 2 or invalid.stdout:
            print('malformed grammar did not fail closed', file=sys.stderr)
            return 1
        after = sorted(path.name for path in directory.iterdir())
        if before != after:
            print('corpus executable wrote to its working directory', file=sys.stderr)
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
