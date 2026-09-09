"""Validate native UI catalogs and produce Qt Linguist sources.

Run without arguments to validate. --inventory prints source strings for review.
--emit-ts DIR produces deterministic TS files for the native build.
"""
from __future__ import annotations

import argparse
import collections
import json
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / "native/i18n/catalog.json"
STRING = r'"(?:[^"\\]|\\.)*"'
STRINGS = rf'(?:{STRING}\s*)+'
CALL = re.compile(rf'(?:QCoreApplication::translate\(\s*"Workbench"\s*,\s*|\b(?:QObject::)?tr\(\s*|QStringLiteral\(\s*)({STRINGS})\)', re.S)
HAN = re.compile(r'[\u3400-\u9fff]')
# Language-independent names, protocol diagnostics mapped to localized messages,
# and filesystem directory names. Never translate these by text matching.
LITERAL_EXEMPTIONS = {
    'Microsoft YaHei UI', 'Yu Gothic UI', 'Segoe UI', 'Invalid scene name',
    'No supported image or video files were found',
    'Gaussian Scene Workbench Backups', 'Gaussian Scene Project',
    'Gaussian Scene Workbench/temporary', 'Gaussian Scene Workbench',
    '3D Gaussian Splatting', '2D Gaussian Splatting',
    'CUDA VMM -> Win32 handle -> OpenGL',
}


def requires_translation(source):
    return source not in LITERAL_EXEMPTIONS and (
        HAN.search(source) or re.search(r'[A-Za-z]{3} [A-Za-z]{2}', source))


def decode_literals(value: str) -> str:
    return ''.join(json.loads(token) for token in re.findall(STRING, value))


def sources():
    result = collections.defaultdict(list)
    for path in sorted((ROOT / 'native/src').glob('*')):
        if path.suffix not in {'.h', '.cpp'} or path.name in {'main.cpp', 'MultiSceneSmokeTest.cpp', 'LanguageSmokeTest.cpp'}:
            continue
        content = path.read_text(encoding='utf-8')
        for match in CALL.finditer(content):
            source = decode_literals(match[1])
            call = match[0]
            if call.startswith('QStringLiteral') and not requires_translation(source):
                continue
            result[source].append((path.relative_to(ROOT).as_posix(), content[:match.start()].count('\n') + 1,
                                   call.startswith('QStringLiteral')))
    return result


def validate(entries, source_map):
    errors = []
    for source, locations in source_map.items():
        if any(location[2] for location in locations):
            errors.append(f'Unlocalized UI literal: {locations[0][:2]} {source!r}')
        if source not in entries:
            errors.append(f'Missing catalog entry: {source!r}')
    for source, translations in entries.items():
        tokens = collections.Counter(re.findall(r'%L?\d+|%n', source))
        for language in ('zh_CN', 'en_US', 'ja_JP'):
            target = translations.get(language, '')
            if not target.strip():
                errors.append(f'Missing {language}: {source!r}')
            elif collections.Counter(re.findall(r'%L?\d+|%n', target)) != tokens:
                errors.append(f'Placeholder mismatch in {language}: {source!r}')
            # QFileDialog wildcards and HTML tags are functional syntax.
            elif re.findall(r'\*[^ ) ;]*', source) != re.findall(r'\*[^ ) ;]*', target):
                errors.append(f'Wildcard mismatch in {language}: {source!r}')
            elif re.findall(r'</?(?:b|br|p|i|strong|em|span|html|body)\b[^>]*>', source) != re.findall(r'</?(?:b|br|p|i|strong|em|span|html|body)\b[^>]*>', target):
                errors.append(f'HTML mismatch in {language}: {source!r}')
    return errors


def load_catalog(path=CATALOG):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f'Duplicate catalog key: {key!r}')
            result[key] = value
        return result
    return json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=unique)


def migrate_literals(entries):
    """One-time mechanical migration, preserving non-UI literals and call arguments."""
    for path in sorted((ROOT / 'native/src').glob('*')):
        if path.suffix not in {'.h', '.cpp'} or path.name in {'main.cpp', 'MultiSceneSmokeTest.cpp', 'LanguageSmokeTest.cpp'}:
            continue
        old = path.read_text(encoding='utf-8')
        def replace(match):
            source = decode_literals(match[1])
            if source in entries and not match[0].startswith('QCoreApplication'):
                return 'QCoreApplication::translate("Workbench", ' + match[1].rstrip() + ')'
            return match[0]
        new = CALL.sub(replace, old)
        if new != old:
            if '#include <QCoreApplication>' not in new:
                new = '#include <QCoreApplication>\n' + new
            path.write_text(new, encoding='utf-8', newline='\n')
            print(f'Migrated {path.relative_to(ROOT)}')


def emit_ts(entries, source_map, destination):
    destination.mkdir(parents=True, exist_ok=True)
    for language in ('zh_CN', 'en_US', 'ja_JP'):
        root = ET.Element('TS', version='2.1', language=language)
        context = ET.SubElement(root, 'context')
        ET.SubElement(context, 'name').text = 'Workbench'
        for source, translations in sorted(entries.items()):
            message = ET.SubElement(context, 'message')
            ET.SubElement(message, 'source').text = source
            ET.SubElement(message, 'translation').text = translations[language]
        ET.indent(root)
        ET.ElementTree(root).write(destination / f'workbench_{language}.ts', encoding='utf-8', xml_declaration=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inventory', action='store_true')
    parser.add_argument('--emit-ts', type=Path)
    parser.add_argument('--migrate-literals', action='store_true')
    args = parser.parse_args()
    entries = load_catalog()
    if args.migrate_literals:
        migrate_literals(entries)
    source_map = sources()
    if args.inventory:
        for i, (source, locations) in enumerate(source_map.items()):
            print(f'{i}\t{locations[0][0]}\t{json.dumps(source, ensure_ascii=False)}')
        return 0
    errors = validate(entries, source_map)
    if errors:
        print('\n'.join(errors), file=sys.stderr)
        return 1
    if args.emit_ts:
        emit_ts(entries, source_map, args.emit_ts)
    print(f'Native i18n: {len(source_map)} UI messages, {len(entries)} catalog entries; zh_CN / en_US / ja_JP complete.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
