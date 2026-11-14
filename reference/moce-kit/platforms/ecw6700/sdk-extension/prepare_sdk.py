"""Prepare an isolated B02P12 SDK without running archive scripts."""
from pathlib import Path, PurePosixPath
import argparse
import hashlib
import json
import shutil
import tarfile


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def safe_relative(value):
    p = PurePosixPath(value)
    if not value or p.is_absolute() or '..' in p.parts or ':' in value or '\\' in value:
        raise SystemExit('Unsafe manifest path: ' + value)
    return p


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--archive', type=Path, help='Fixed input archive; defaults to the adjacent manifest filename')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    spec = json.loads((root / 'SDK_SOURCE.json').read_text(encoding='utf-8'))
    safe_relative(spec['archive'])
    archive = args.archive.resolve() if args.archive else root / spec['archive']
    safe_relative(spec['sdk_root'])
    for entry in spec['overlays']:
        safe_relative(entry['path'])
        if sha(root / 'overlay' / entry['path']) != entry['after']:
            raise SystemExit('Overlay changed: ' + entry['path'])
    if sha(archive) != spec['sha256']:
        raise SystemExit('SDK archive hash mismatch')
    output = args.output.resolve()
    if output.exists():
        raise SystemExit('Output must not exist; never overwrite an installed SDK')
    with tarfile.open(archive) as source:
        members = source.getmembers()
        for entry in members:
            path = PurePosixPath(entry.name)
            if path.is_absolute() or '..' in path.parts or ':' in entry.name or '\\' in entry.name:
                raise SystemExit('Unsafe archive path: ' + entry.name)
            if not (entry.isfile() or entry.isdir()):
                raise SystemExit('Unsupported archive entry: ' + entry.name)
        output.mkdir(parents=True)
        for entry in members:
            target = output.joinpath(*PurePosixPath(entry.name).parts)
            if entry.isdir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with source.extractfile(entry) as src, target.open('wb') as dst:
                    shutil.copyfileobj(src, dst)
    code = output / spec['sdk_root'] / 'code'
    for entry in spec['overlays']:
        target = code / entry['path']
        payload = root / 'overlay' / entry['path']
        if sha(payload) != entry['after']:
            raise SystemExit('Overlay changed: ' + entry['path'])
        before = entry['before']
        if before is not None and (not target.is_file() or sha(target) != before):
            raise SystemExit('Wrong SDK input: ' + entry['path'])
        if before is None and target.exists():
            raise SystemExit('Unexpected SDK addition collision: ' + entry['path'])
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(payload, target)
    (output / 'PREPARED.json').write_text(json.dumps(spec, indent=2, ensure_ascii=False), encoding='utf-8')
    print(code)


if __name__ == '__main__':
    main()
