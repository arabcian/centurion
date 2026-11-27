#!/usr/bin/env python3
"""The Gentoo Manifest must list every ebuild and metadata file with its current size and digests,
or emerge refuses the package ("Manifest verification failed").

    python3 tests/gentoo_manifest_static_test.py            check (CI)
    python3 tests/gentoo_manifest_static_test.py --update   rewrite the EBUILD/MISC/AUX lines

DIST lines (release tarballs) are kept as they are: the tarball is not in the repository.
"""
import hashlib
import sys
from pathlib import Path

PKG = Path(__file__).resolve().parents[1] / 'packaging/gentoo/sys-power/centurion'
MANIFEST = PKG / 'Manifest'


def entry(kind, path, name):
    data = path.read_bytes()
    return f'{kind} {name} {len(data)} BLAKE2B {hashlib.blake2b(data).hexdigest()} SHA512 {hashlib.sha512(data).hexdigest()}'


def expected(dist_lines):
    lines = list(dist_lines)
    for f in sorted(PKG.glob('*.ebuild')):
        lines.append(entry('EBUILD', f, f.name))
    files = PKG / 'files'
    if files.is_dir():
        for f in sorted(p for p in files.rglob('*') if p.is_file()):
            lines.append(entry('AUX', f, str(f.relative_to(files))))
    lines.append(entry('MISC', PKG / 'metadata.xml', 'metadata.xml'))
    return lines


def main():
    current = MANIFEST.read_text(encoding='utf-8').splitlines()
    dist = [line for line in current if line.startswith('DIST ')]
    want = expected(dist)
    if '--update' in sys.argv[1:]:
        MANIFEST.write_text('\n'.join(sorted(want)) + '\n', encoding='utf-8')
        print(f'updated {MANIFEST}')
        return
    have, need = set(current), set(want)
    names = {line.split()[1] for line in want}
    stale = sorted(line.split()[1] for line in need - have)
    extra = sorted(line.split()[1] for line in have - need if line.split()[1] not in names)
    if stale or extra:
        raise SystemExit('Gentoo Manifest is out of date (stale or missing: %s; no such file: %s).\n'
                         'Run: python3 tests/gentoo_manifest_static_test.py --update'
                         % (', '.join(stale) or '-', ', '.join(extra) or '-'))
    print('PASS: Gentoo Manifest matches every ebuild and metadata file')


if __name__ == '__main__':
    main()
