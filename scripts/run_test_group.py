"""Run a named GTest release subset, refusing filters that select no assertions."""
import subprocess
import sys


def main():
    executable, group = sys.argv[1:]
    prefix = '--gtest_filter='
    if group.startswith(prefix):
        group = group[len(prefix):]
    listed = subprocess.run([executable, '--gtest_list_tests', '--gtest_filter=' + group],
                            capture_output=True, text=True, timeout=15)
    names = [line for line in listed.stdout.splitlines()
             if line.startswith('  ') and line.strip() and not line.lstrip().startswith('#')]
    if listed.returncode or not names:
        print('Required release subset selected no tests', file=sys.stderr)
        return 1
    return subprocess.run([executable, '--gtest_filter=' + group], timeout=100).returncode


if __name__ == '__main__':
    sys.exit(main())
