"""Offline consistency of public build metadata and executable version fields."""
import json
from pathlib import Path
import subprocess
import sys


def main():
    emitter, daemon, cli, source = map(Path, sys.argv[1:])
    expected = (source / 'VERSION').read_text(encoding='utf-8').strip()
    metadata = json.loads(subprocess.check_output([str(emitter)], timeout=10))
    if metadata['version'] != expected:
        raise ValueError('metadata differs from central version')
    for executable in (daemon, cli):
        output = subprocess.check_output([str(executable), '--version'], timeout=10).decode().split()
        if output[1] != expected or output[2] != f"revision={metadata['git_commit']}":
            raise ValueError('executable metadata differs')
    for key in ('compiler', 'target_platform', 'configuration'):
        if not metadata[key] or metadata[key] == 'UNRECORDED':
            raise ValueError('configured build input is missing')
    if metadata['build_timestamp'] is not None and not isinstance(metadata['build_timestamp'], int):
        raise ValueError('timestamp must be omitted/null or an explicit UTC epoch')
    if set(metadata['networks']) != {'regtest', 'testnet', 'mainnet'}:
        raise ValueError('network metadata inventory differs')
    for network in metadata['networks'].values():
        if not 0 < network['minimum_peer_version'] <= network['protocol_version']:
            raise ValueError('invalid network compatibility metadata')
    print('central version, executable and build metadata consistency PASS')


if __name__ == '__main__':
    main()
