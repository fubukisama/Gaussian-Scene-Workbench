"""Stage the native README with only validated, package-local document links."""
import argparse
import json
from pathlib import Path
import re
import shutil


LINK = re.compile(r'\]\(([^)]+)\)')
DOCUMENT = re.compile(r'^\.\./docs/([A-Za-z0-9_.-]+\.md)(#[^)]*)?$')


def stage_readme(source_root, package_root):
    source_root, package_root = Path(source_root).resolve(), Path(package_root).resolve()
    source = source_root / 'native/README.md'
    text = source.read_text(encoding='utf-8')
    files = {}

    def replace(match):
        target = match.group(1)
        if target.startswith(('https://', 'http://', '#')):
            return match.group(0)
        document = DOCUMENT.fullmatch(target)
        if document:
            relative = 'docs/' + document.group(1)
            anchor = document.group(2) or ''
        elif target in ('../COPYING', '../LICENSE', '../THIRD_PARTY_LICENSES.md'):
            relative, anchor = target[3:], ''
        elif target == 'LICENSE.md':
            relative, anchor = 'native/LICENSE.md', ''
        else:
            raise ValueError('unsupported native README parent link: ' + target)
        original = source_root / relative
        if not original.is_file():
            raise ValueError('missing native README document: ' + relative)
        original.resolve().relative_to(source_root)
        files[relative] = original
        return '](' + relative + anchor + ')'

    output = LINK.sub(replace, text)
    # Validate every source before creating or changing package documents.
    for relative, original in sorted(files.items()):
        destination = package_root / relative
        destination.resolve().relative_to(package_root)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(str(original), str(destination))
    package_root.mkdir(parents=True, exist_ok=True)
    with (package_root / 'README.md').open('w', encoding='utf-8', newline='') as stream:
        stream.write(output)
    return {'schema': 'gsw.package_readme_links/1', 'copied_links': sorted(files)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--package-root', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(stage_readme(args.source_root, args.package_root)))


if __name__ == '__main__':
    main()
