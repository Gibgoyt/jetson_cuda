#!/usr/bin/env python3
"""
cpp_reformat.py -- reformat a C/C++ file to Ahmed's house style.

Pipeline per file:
  1. Refuse if the file is HTML (defensive check for accidentally-saved web pages).
  2. Record baseline `g++ -fsyntax-only` error count.
  3. Run `clang-format` with an INLINE style (so this script does not depend on
     whichever `.clang-format` is in scope on disk).
  4. Python post-pass: re-indent the body of every `#if`/`#endif` block one tab
     deeper than the directive. Nesting-aware. `#else`/`#elif` sit at the
     indent of their `#if`. ONLY prepends tabs to leading whitespace -- never
     touches a character past the first non-whitespace position, so it cannot
     change C++ tokenization.
  5. Re-run `g++ -fsyntax-only`. If the error count went UP, the file is
     restored with `git checkout HEAD -- <file>` and a regression is reported.

CLI:
  ./Scripts/cpp_reformat.py FILE [FILE ...]
  --check       : write the reformatted file, run g++ check, then restore the
                  original from the on-disk backup (does NOT touch git).
  --no-restore  : do not auto-restore on regression (default: restore from HEAD).
  --std=c++NN   : standard for g++ check (default: c++20).
"""

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


CLANG_FORMAT_STYLE = """{
  BasedOnStyle: LLVM,
  Language: Cpp,
  Standard: c++17,
  ColumnLimit: 100,
  IndentWidth: 4,
  TabWidth: 4,
  UseTab: ForIndentation,
  AccessModifierOffset: -4,
  AlignAfterOpenBracket: BlockIndent,
  BinPackParameters: false,
  BinPackArguments: false,
  AllowAllParametersOfDeclarationOnNextLine: false,
  AllowAllArgumentsOnNextLine: false,
  AllowShortFunctionsOnASingleLine: Inline,
  AllowShortIfStatementsOnASingleLine: Never,
  AllowShortLoopsOnASingleLine: false,
  AlwaysBreakTemplateDeclarations: Yes,
  BreakBeforeBraces: Attach,
  Cpp11BracedListStyle: true,
  DerivePointerAlignment: false,
  FixNamespaceComments: true,
  IncludeBlocks: Preserve,
  NamespaceIndentation: None,
  PointerAlignment: Left,
  SortIncludes: false,
  SpaceAfterTemplateKeyword: true,
  SpacesBeforeTrailingComments: 2,
  IndentPPDirectives: BeforeHash,
  PenaltyReturnTypeOnItsOwnLine: 200,
  ConstructorInitializerIndentWidth: 4,
  ContinuationIndentWidth: 4
}"""


HTML_MARKERS = ('<!doctype html', '<html ', '<html>')


def looks_like_html(text: str) -> bool:
    head = text[:4000].lower()
    return any(m in head for m in HTML_MARKERS)


def run(cmd, **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, **kwargs)


def gpp_error_count(path: Path, std: str) -> tuple[int, int, str]:
    result = run([
        'g++', '-fsyntax-only', f'-std={std}', '-w', '-x', 'c++', str(path),
    ])
    errs = len(re.findall(r': error:', result.stderr))
    return result.returncode, errs, result.stderr


def clang_format_inplace(path: Path) -> None:
    name = path.name
    if not any(name.endswith(e) for e in ('.cpp', '.hpp', '.h', '.cc', '.cxx', '.cppm')):
        assume = name + '.hpp'
    else:
        assume = name
    content = path.read_text(encoding='utf-8')
    result = run(
        ['clang-format', f'--style={CLANG_FORMAT_STYLE}', f'--assume-filename={assume}'],
        input=content,
    )
    if result.returncode != 0:
        raise RuntimeError(f'clang-format failed on {path}:\n{result.stderr}')
    path.write_text(result.stdout, encoding='utf-8')


# Match a PP directive at the start of a (possibly indented) line.
PP_RE = re.compile(r'^[ \t]*#\s*(\w+)')

# Detect entry to a raw string literal that is not closed on the same line.
RAW_OPEN_RE = re.compile(r'R"([^()\\\s]{0,16})\(')


def post_pass_indent_pp_bodies(text: str) -> str:
    """
    Add `pp_depth` tabs to the start of every line that is inside a
    `#if`/`#endif` block. The directives themselves are placed at the indent
    of their enclosing block (so `#if` at depth N goes at N tabs from the
    PP-pass; `#endif` at N tabs; `#else`/`#elif` at N tabs; everything
    between is at N+1 tabs).

    Lines inside multi-line raw string literals are passed through unchanged
    -- adding tabs would corrupt the string contents. Block comments and
    other code are safe to tab-prepend because C++ is whitespace-insensitive
    at the start of every line outside a raw string.
    """
    out = []
    pp_depth = 0
    raw_close = None  # the closing token of an open raw string, e.g. ')xyz"' -- or None

    for raw_line in text.splitlines(keepends=True):
        nl = ''
        body = raw_line
        if body.endswith('\r\n'):
            nl, body = '\r\n', body[:-2]
        elif body.endswith('\n'):
            nl, body = '\n', body[:-1]

        # Inside a raw string -- do not touch.
        if raw_close is not None:
            out.append(body + nl)
            if raw_close in body:
                raw_close = None
            continue

        # Detect a raw string opening on this line.
        m = RAW_OPEN_RE.search(body)
        if m:
            close = ')' + m.group(1) + '"'
            after = body[m.end():]
            if close not in after:
                # The opening line is still ordinary code up to the `R"(` --
                # safe to prepend tabs to the leading whitespace.
                raw_close = close

        m = PP_RE.match(body)
        if m is not None:
            # clang-format with `IndentPPDirectives: BeforeHash` already places
            # every PP directive (including nested ones) at the right indent.
            # We pass directives through untouched and only update pp_depth so
            # subsequent NON-directive (code) lines get the extra tab.
            directive = m.group(1)
            if directive in ('if', 'ifdef', 'ifndef'):
                out.append(body + nl)
                pp_depth += 1
            elif directive == 'endif':
                pp_depth = max(0, pp_depth - 1)
                out.append(body + nl)
            else:
                # #else, #elif, #define, #include, #pragma, #error, #line, #undef, ...
                out.append(body + nl)
            continue

        # Ordinary code/comment line -- prepend pp_depth tabs (if non-blank).
        prefix = '\t' * pp_depth if body.strip() else ''
        out.append(prefix + body + nl)

    return ''.join(out)


def git_restore_head(path: Path) -> None:
    run(['git', 'checkout', 'HEAD', '--', str(path)])


def process_file(path: Path, *, check: bool, restore: bool, std: str) -> int:
    if not path.is_file():
        print(f'[skip] {path}: not a regular file', file=sys.stderr)
        return 2

    text = path.read_text(encoding='utf-8', errors='replace')

    if looks_like_html(text):
        print(f'[skip] {path}: file appears to be HTML, not C/C++ -- refusing to format')
        return 2

    print(f'[+] {path}')

    rc0, errs0, _ = gpp_error_count(path, std)
    print(f'    baseline g++:   exit={rc0:>3}, error: count={errs0}')

    backup = Path(tempfile.mkstemp(prefix=f'reformat_{path.stem}_', suffix='.orig')[1])
    backup.write_text(text, encoding='utf-8')

    try:
        clang_format_inplace(path)
        after_cf = path.read_text(encoding='utf-8')
        final = post_pass_indent_pp_bodies(after_cf)
        path.write_text(final, encoding='utf-8')

        rc1, errs1, stderr1 = gpp_error_count(path, std)
        print(f'    reformat g++:   exit={rc1:>3}, error: count={errs1}')

        if errs1 > errs0:
            print(f'    [REGRESSION] error count {errs0} -> {errs1}')
            sample = stderr1.splitlines()[:25]
            for ln in sample:
                print(f'      | {ln}')
            if restore:
                git_restore_head(path)
                print(f'    restored: git checkout HEAD -- {path}')
            return 1

        if check:
            # rollback to backup
            shutil.copy(backup, path)
            print('    [check mode] file rolled back to original')
        return 0
    finally:
        try:
            backup.unlink()
        except OSError:
            pass


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description='Reformat C/C++ files to Ahmed house style.')
    ap.add_argument('files', nargs='+', type=Path)
    ap.add_argument('--check', action='store_true',
                    help='write reformatted file then roll back to on-disk backup')
    ap.add_argument('--no-restore', action='store_true',
                    help='do not git-restore on regression')
    ap.add_argument('--std', default='c++20', help='C++ standard for g++ check')
    args = ap.parse_args(argv)

    worst = 0
    for f in args.files:
        rc = process_file(
            f,
            check=args.check,
            restore=not args.no_restore,
            std=args.std,
        )
        worst = max(worst, rc)
    return worst


if __name__ == '__main__':
    sys.exit(main())
