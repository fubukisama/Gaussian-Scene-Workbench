"""Fast, dependency-free checks for GSW's public documentation entry points.

Markdown source soft wrapping is valid. Only the plain-text LICENSE is required
to keep each prose paragraph on one physical line, so GitHub can wrap it once.
"""
from __future__ import annotations

import argparse
from html.parser import HTMLParser
import json
from pathlib import Path
import posixpath
import re
import subprocess
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[2]
PRIMARY = (
    "README.md", "README.zh-CN.md", "README.ja.md", "README_RELEASE.md",
    "native/README.md", "native/LICENSE.md", "docs/README.md",
    "docs/LICENSING.md", "docs/images/README.md",
)
LANGUAGE_PAGES = {"README.md": "en", "README.zh-CN.md": "zh-CN", "README.ja.md": "ja"}
OVERVIEW_BADGE_TARGETS = {
    "README.md": ("download-the-preview", "before-you-start"),
    "README.zh-CN.md": ("下载预览版", "使用前确认"),
    "README.ja.md": ("プレビュー版の入手", "利用前の確認"),
}
MULTILINGUAL = {"README_RELEASE.md", "docs/README.md", "docs/LICENSING.md", "native/README.md", "docs/images/README.md", "docs/TRAINING_PERFORMANCE.md"}


class Html(HTMLParser):
    def __init__(self):
        super().__init__()
        self.links = []
        self.errors = []
        self.details = 0
        self.divs = 0
        self.languages = set()
        self.anchor_stack = []
        self.linked_images = []

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == "a":
            self.anchor_stack.append(attrs.get("href", ""))
        if tag == "img":
            self.linked_images.append((attrs.get("src", ""), self.anchor_stack[-1] if self.anchor_stack else ""))
        if attrs.get("lang"):
            self.languages.add(attrs["lang"])
        if tag == "div":
            self.divs += 1
        if tag == "details":
            self.details += 1
        if tag == "summary" and self.details == 0:
            self.errors.append("summary appears outside details")
        if tag == "img" and not attrs.get("alt", "").strip():
            self.errors.append("image has no alt text")
        for key in ("href", "src"):
            if attrs.get(key):
                self.links.append(attrs[key])

    def handle_endtag(self, tag):
        if tag == "a" and self.anchor_stack:
            self.anchor_stack.pop()
        if tag == "div":
            self.divs -= 1
            if self.divs < 0:
                self.errors.append("unmatched div closing tag")
        if tag == "details":
            self.details -= 1
            if self.details < 0:
                self.errors.append("unmatched details closing tag")


def without_fences(text, errors):
    result = []
    fence = None
    for number, line in enumerate(text.splitlines(), 1):
        match = re.match(r"^\s{0,3}(`{3,}|~{3,})(.*)$", line)
        if match:
            token, rest = match.groups()
            if fence is None:
                fence = (token[0], len(token), number)
                result.append("")
                continue
            if token[0] == fence[0] and len(token) >= fence[1] and not rest.strip():
                fence = None
                result.append("")
                continue
        result.append("" if fence else line)
    if fence:
        errors.append(f"line {fence[2]}: unclosed code fence")
    return result


def plain_license_errors(text):
    errors = []
    lines = text.splitlines()
    index = 0
    def heading(at):
        return at + 1 < len(lines) and bool(lines[at].strip()) and bool(re.fullmatch(r"[=-]+", lines[at + 1]))
    while index < len(lines):
        if not lines[index].strip():
            index += 1
            continue
        if heading(index):
            index += 2
            continue
        start = index
        while index < len(lines) and lines[index].strip() and not heading(index):
            index += 1
        count = index - start
        if count > 1:
            errors.append(f"line {start+1}: prose paragraph has {count} hard-wrapped lines; join the paragraph and let the viewer wrap it")
    return errors


def tracked_files():
    result=subprocess.run(["git","ls-files","-z"],cwd=ROOT,check=True,capture_output=True,timeout=10)
    return {path for path in result.stdout.decode("utf-8").split("\0") if path}


def link_destination(target, html_attribute=False):
    target=target.strip()
    if target.startswith("<") and ">" in target:
        return target[1:target.index(">")]
    if not html_attribute:
        target=re.split(r"(?<!\\)\s+",target,maxsplit=1)[0]
        return target.replace("\\ "," ")
    return target


def check_document(relative, tracked=None):
    if tracked is None:tracked=tracked_files()
    path = ROOT / relative
    errors = []
    text = path.read_text(encoding="utf-8-sig")
    if "\ufffd" in text:
        errors.append("replacement character in UTF-8 text")
    lines = without_fences(text, errors)
    clean = "\n".join(lines)
    parser = Html(); parser.feed(clean)
    errors.extend(parser.errors)
    if parser.details:
        errors.append("unclosed details block")
    if parser.divs:
        errors.append("unclosed language/container div")
    if relative in LANGUAGE_PAGES and not re.match(r'\s*<div lang="'+re.escape(LANGUAGE_PAGES[relative])+r'">', text):
        errors.append("localized overview has no matching document-language container")
    if relative in OVERVIEW_BADGE_TARGETS:
        release_guide = (ROOT / "README_RELEASE.md").read_text(encoding="utf-8-sig")
        for badge, fragment in zip(("native_preview", "platform"), OVERVIEW_BADGE_TARGETS[relative]):
            matches = [(src, href) for src, href in parser.linked_images if urlsplit(src).path.startswith("/badge/" + badge + "-") and urlsplit(src).netloc == "img.shields.io"]
            expected = "README_RELEASE.md#" + fragment
            if len(matches) != 1 or matches[0][1] != expected:
                errors.append(f"{badge} badge must link to its localized guide section: {expected}")
            headings = re.findall(r"^#{1,6}\s+(.+?)\s*$", release_guide, re.M)
            if fragment not in {heading.lower().replace(" ", "-") for heading in headings}:
                errors.append(f"{badge} badge target section does not exist: {expected}")
    if relative in MULTILINGUAL and not {"en", "zh-CN", "ja"}.issubset(parser.languages):
        errors.append("multilingual entry lacks explicit English/Chinese/Japanese language markers")
    if re.search(r'<font\b|style\s*=[^>]*font-family', clean, re.I):
        errors.append("do not rely on GitHub-filtered font overrides; use semantic language markers")
    for number, line in enumerate(lines, 1):
        no_code = re.sub(r"`[^`]*`", "", line)
        if re.search(r"[A-Za-z0-9]\[[^\]\n]+\]\([^\n)]+\)", no_code):
            errors.append(f"line {number}: English text is joined directly to a Markdown link; add a space")
        if re.match(r"^日本語[:：]", line) and number > 1 and re.match(r"^中文[:：]", lines[number-2]):
            errors.append(f"line {number}: Chinese and Japanese paragraphs need a blank separator")
    if relative in MULTILINGUAL:
        language_stack=[]
        expected_language={"English":"en","中文":"zh-CN","简体中文":"zh-CN","日本語":"ja"}
        for number,line in enumerate(lines,1):
            for tag in re.finditer(r"<(/?)div\b([^>]*)>",line):
                if tag[1]:
                    if language_stack:language_stack.pop()
                else:
                    language=re.search(r'lang="([^"]+)"',tag[2])
                    language_stack.append(language[1] if language else (language_stack[-1] if language_stack else None))
            heading=re.match(r"^#{1,6}\s+(English|中文|简体中文|日本語)\s*$",line)
            if heading and (not language_stack or language_stack[-1]!=expected_language[heading[1]]):
                errors.append(f"line {number}: language section inherits the wrong language")
    def cells(line):
        parts = re.split(r"(?<!\\)\|", line.strip())
        if parts and not parts[0].strip():parts=parts[1:]
        if parts and not parts[-1].strip():parts=parts[:-1]
        return parts
    for index,line in enumerate(lines):
        columns=cells(line)
        if len(columns)<2 or not all(re.fullmatch(r"\s*:?-{3,}:?\s*",cell) for cell in columns):continue
        if index==0 or len(cells(lines[index-1]))!=len(columns):
            errors.append(f"line {index+1}: table header/separator column mismatch")
        if index>=2 and lines[index-2].strip():
            errors.append(f"line {index}: add a blank line before the table")
        row=index+1
        while row<len(lines) and lines[row].strip() and "|" in lines[row]:
            if len(cells(lines[row]))!=len(columns):errors.append(f"line {row+1}: table row column count differs from header")
            row+=1
    links = [(target,False) for target in re.findall(r"!?\[[^\]\n]*\]\(([^\n)]+)\)", clean)] + [(target,True) for target in parser.links]
    for target,is_html in links:
        target=link_destination(target,is_html)
        url = urlsplit(target)
        if url.scheme or url.netloc or not url.path:
            continue
        selected = (path.parent / unquote(url.path)).resolve()
        if not selected.is_relative_to(ROOT) or not selected.exists():
            errors.append(f"missing/escaping local link: {target}")
            continue
        public_target=posixpath.normpath(posixpath.join(posixpath.dirname(relative),unquote(url.path)))
        published=(public_target in tracked or public_target=="." or any(item.startswith(public_target.rstrip("/")+"/") for item in tracked))
        if not published:errors.append(f"local link target is not tracked/published: {target}")
    return errors, len(links)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    paths = sorted(set(PRIMARY) | {p.relative_to(ROOT).as_posix() for p in (ROOT / "docs").glob("*.md")})
    findings = []
    references = 0
    tracked=tracked_files()
    for relative in paths:
        errors, count = check_document(relative,tracked)
        references += count
        findings.extend({"path": relative, "message": error} for error in errors)
    findings.extend({"path": "LICENSE", "message": error} for error in plain_license_errors((ROOT / "LICENSE").read_text(encoding="utf-8")))
    report = {"checkedMarkdownFiles": len(paths), "checkedLinkReferences": references,
              "plainTextLicenseChecked": True, "publishedTargetInventoryChecked":True, "standardGplTextReformatted": False,
              "linkedOverviewBadgesChecked": True,
              "findings": findings, "passed": not findings}
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        with args.report.open("x", encoding="utf-8") as handle:
            json.dump(report, handle, ensure_ascii=False, indent=2)
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return bool(findings)


if __name__ == "__main__":
    raise SystemExit(main())
