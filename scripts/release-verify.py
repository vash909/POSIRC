#!/usr/bin/env python3
"""Check the complete binary asset set before a beta becomes public."""
import json
import pathlib
import re
import sys
import tarfile


def verify_assets(directory, revision):
    version = re.search(r'^#define POSIRC_VERSION "([^"]+)"',
                        pathlib.Path('include/app.h').read_text(), re.M)[1]
    targets = json.loads(pathlib.Path('scripts/release-targets.json').read_text())
    expected = {'posirc-%s-%s.tar.gz' % (version, row['target']): row['target']
                for row in targets}
    assert len(expected) == len(targets), 'Duplicate release target'
    actual = {p.name for p in pathlib.Path(directory).glob('*.tar.gz')}
    assert actual == expected.keys(), ('Archive set mismatch', sorted(actual ^ expected.keys()))
    for name, target in sorted(expected.items()):
        root = name.removesuffix('.tar.gz')
        with tarfile.open(pathlib.Path(directory) / name, 'r:gz') as archive:
            binary = archive.getmember(root + '/posirc')
            assert binary.isfile() and binary.size > 0 and binary.mode & 0o111, name
            for required in ['README.md', 'LICENSE', 'INSTALL.txt', 'BUILD-INFO.txt']:
                assert archive.getmember(root + '/' + required).isfile(), (name, required)
            info = archive.extractfile(root + '/BUILD-INFO.txt').read().decode().splitlines()
            for line in ['Source commit: ' + revision, 'POSIRC version: ' + version,
                         'Target: ' + target]:
                assert line in info, (name, line)
        print('Verified:', name)
    print('Complete release:', len(expected), 'tested binary archives; source:', revision)


if __name__ == '__main__':
    if len(sys.argv) != 4 or sys.argv[1] != 'assets':
        raise SystemExit('usage: release-verify.py assets DIST SOURCE_SHA')
    verify_assets(sys.argv[2], sys.argv[3])
