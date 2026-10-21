"""Calibrate Linux syscall denial, then guard the standalone version path."""
import os
from pathlib import Path
import resource
import signal
import subprocess
import sys
import tempfile


def main():
    guard, probe, *executables = map(lambda p: str(Path(p).resolve()), sys.argv[1:])
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    env = dict(os.environ, LD_PRELOAD=guard, TZ='UTC0')
    with tempfile.TemporaryDirectory(prefix='nova-version-guard-') as directory:
        for mode in ('socket', 'file'):
            result = subprocess.run([probe, mode], env=env, cwd=directory,
                                    capture_output=True, timeout=5)
            assert result.returncode == -signal.SIGSYS, result
        for executable in executables:
            result = subprocess.run([executable, '--version'], env=env, cwd=directory,
                                    capture_output=True, timeout=5)
            assert result.returncode == 0 and not result.stderr, result
            assert len(result.stdout) < 512 and b' revision=' in result.stdout
        assert not list(Path(directory).iterdir())
    print('calibrated Linux post-loader file/network syscall guard PASS')


if __name__ == '__main__':
    main()
