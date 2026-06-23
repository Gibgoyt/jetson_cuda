#!/usr/bin/env python3
"""
cpp_reformat.py -- reformat C/C++ files to Ahmed's house style.

Two modes:

  LIST MODE (default):
    ./Scripts/cpp_reformat.py
        Reads ./Scripts/cpp_reformat_files.txt, runs a per-file
        `git restore --` on every entry that passes the C/C++ filter
        (so each file starts from a clean HEAD state), then formats them.
        On the first regression: print the file and g++ stderr, leave the
        corrupted file on disk, exit non-zero.

  AD-HOC MODE:
    ./Scripts/cpp_reformat.py FILE [FILE ...]
        Formats just those files. No pre-flight git restore.

Per-file pipeline:
  1. Refuse if the file looks like HTML (defensive check).
  2. For non-CUDA: record baseline `g++ -fsyntax-only` error count.
  3. Run `clang-format` with an INLINE style (so this script does not depend on
     whichever `.clang-format` is in scope on disk).
  4. Python post-pass: re-indent the body of every `#if`/`#endif` block one tab
     deeper than the directive. Nesting-aware. `#else`/`#elif` sit at the
     indent of their `#if`. ONLY prepends tabs to leading whitespace -- never
     touches a character past the first non-whitespace position, so it cannot
     change C++ tokenization.
  5. For non-CUDA: re-run `g++ -fsyntax-only`. If the error count went UP,
     print the file path + first ~50 lines of stderr, leave the file on disk,
     and exit immediately. The user is expected to inspect it manually and
     fix this script before running again.

CUDA files (.cu/.cuh) get formatted but the g++ check is skipped (g++ does not
understand `__global__`, `<<<...>>>`, etc.).

Vendored third-party files (utils/image/stb/*, utils/json.hpp) are skipped
entirely so we never diff against upstream.
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

DEFAULT_LIST = Path('./Scripts/cpp_reformat_files.txt')

CPP_EXTS = {'.cpp', '.cc', '.cxx', '.cppm', '.hpp', '.h', '.inl'}
CUDA_EXTS = {'.cu', '.cuh'}
FORMAT_EXTS = CPP_EXTS | CUDA_EXTS

# Path fragments / exact relpaths for vendored / upstream code we must NOT touch.
VENDORED_PREFIXES = (
    'utils/image/stb/',
)
VENDORED_EXACT = {
    'utils/json.hpp',
}


def looks_like_html(text: str) -> bool:
    head = text[:4000].lower()
    return any(m in head for m in HTML_MARKERS)


def run(cmd, **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, **kwargs)


def normalize_relpath(p: Path) -> str:
    s = str(p)
    if s.startswith('./'):
        s = s[2:]
    return s


def is_vendored(p: Path) -> bool:
    rel = normalize_relpath(p)
    if rel in VENDORED_EXACT:
        return True
    return any(rel.startswith(prefix) for prefix in VENDORED_PREFIXES)


def classify(p: Path) -> str:
    """Return one of: 'cpp', 'cuda', 'vendored', 'skip'."""
    if is_vendored(p):
        return 'vendored'
    ext = p.suffix.lower()
    if ext in CUDA_EXTS:
        return 'cuda'
    if ext in CPP_EXTS:
        return 'cpp'
    return 'skip'


def gpp_error_count(path: Path, std: str) -> tuple[int, int, str]:
    result = run([
        'g++', '-fsyntax-only', f'-std={std}', '-w', '-x', 'c++', str(path),
    ])
    # Count distinct diagnostic lines with `error:` or `fatal error:`.
    # g++ emits these as: `<file>:<line>:<col>: error: ...` or `... fatal error: ...`
    errs = len(re.findall(r':\s*(?:fatal\s+)?error:', result.stderr))
    return result.returncode, errs, result.stderr


def clang_format_inplace(path: Path) -> None:
    name = path.name
    if not any(name.endswith(e) for e in ('.cpp', '.hpp', '.h', '.cc', '.cxx', '.cppm', '.cu', '.cuh', '.inl')):
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


def read_list_file(list_path: Path) -> list[Path]:
    raw = list_path.read_text(encoding='utf-8').splitlines()
    paths = []
    for line in raw:
        s = line.strip()
        if not s or s.startswith('#'):
            continue
        paths.append(Path(s))
    return paths


def preflight_git_restore(paths: list[Path]) -> None:
    """
    For every file in `paths` that we are about to format, run
    `git restore -- <file>`. This guarantees we start from HEAD state.

    We deliberately do this per-file (never `git reset`, never a wildcard
    `git checkout HEAD -- '*'`) so any WIP the user has in OTHER files
    is left alone.
    """
    print(f'[preflight] git restore -- (x{len(paths)} files)')
    failed = 0
    for p in paths:
        if not p.exists():
            # Untracked / never existed -- nothing to restore. Skip silently.
            continue
        r = run(['git', 'restore', '--', str(p)])
        if r.returncode != 0:
            failed += 1
            print(f'  [warn] git restore failed for {p}: {r.stderr.strip()}', file=sys.stderr)
    if failed:
        print(f'[preflight] {failed} restore(s) failed (continuing)', file=sys.stderr)
    else:
        print('[preflight] all restores ok')


class Regression(Exception):
    def __init__(self, path: Path, baseline: int, after: int, stderr: str):
        super().__init__(f'regression on {path}: {baseline} -> {after}')
        self.path = path
        self.baseline = baseline
        self.after = after
        self.stderr = stderr


def process_file(
    path: Path,
    *,
    check: bool,
    std: str,
    skip_gpp: bool,
) -> str:
    """
    Returns a short status string:
      'ok'         - reformatted, no error count change
      'kept'       - reformatted, baseline already had errors, count preserved
      'cuda'       - reformatted, g++ check skipped
      'html-skip'  - file looked like HTML, refused
    Raises Regression on increase in g++ error count.
    """
    if not path.is_file():
        print(f'[skip] {path}: not a regular file', file=sys.stderr)
        return 'missing'

    text = path.read_text(encoding='utf-8', errors='replace')

    if looks_like_html(text):
        print(f'[skip] {path}: file appears to be HTML, not C/C++ -- refusing to format')
        return 'html-skip'

    label = '[+cuda]' if skip_gpp else '[+]'
    print(f'{label} {path}')

    if skip_gpp:
        rc0, errs0 = 0, 0
    else:
        rc0, errs0, _ = gpp_error_count(path, std)
        print(f'    baseline g++:   exit={rc0:>3}, error count={errs0}')

    backup = Path(tempfile.mkstemp(prefix=f'reformat_{path.stem}_', suffix='.orig')[1])
    backup.write_text(text, encoding='utf-8')

    try:
        clang_format_inplace(path)
        after_cf = path.read_text(encoding='utf-8')
        final = post_pass_indent_pp_bodies(after_cf)
        path.write_text(final, encoding='utf-8')

        if skip_gpp:
            if check:
                shutil.copy(backup, path)
                print('    [check mode] file rolled back to original')
            return 'cuda'

        rc1, errs1, stderr1 = gpp_error_count(path, std)
        print(f'    reformat g++:   exit={rc1:>3}, error count={errs1}')

        if errs1 > errs0:
            raise Regression(path, errs0, errs1, stderr1)

        if check:
            shutil.copy(backup, path)
            print('    [check mode] file rolled back to original')

        return 'kept' if errs0 > 0 else 'ok'
    finally:
        try:
            backup.unlink()
        except OSError:
            pass


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description='Reformat C/C++ files to Ahmed house style.')
    ap.add_argument('files', nargs='*', type=Path,
                    help='ad-hoc per-file mode; if empty, list mode is used')
    ap.add_argument('--from-list', type=Path, default=DEFAULT_LIST,
                    help=f'path to file list (default: {DEFAULT_LIST})')
    ap.add_argument('--check', action='store_true',
                    help='write reformatted file then roll back to on-disk backup')
    ap.add_argument('--keep-going', action='store_true',
                    help='do not halt on the first regression (still leaves files on disk)')
    ap.add_argument('--no-preflight-restore', action='store_true',
                    help='in list mode, skip the per-file `git restore --` pre-flight')
    ap.add_argument('--std', default='c++20', help='C++ standard for g++ check')
    args = ap.parse_args(argv)

    # Mode selection: explicit files -> ad-hoc; otherwise list mode.
    if args.files:
        ad_hoc_files = list(args.files)
        list_mode = False
    else:
        if not args.from_list.is_file():
            print(f'error: list file not found: {args.from_list}', file=sys.stderr)
            return 2
        ad_hoc_files = read_list_file(args.from_list)
        list_mode = True
        print(f'[list-mode] reading {args.from_list} ({len(ad_hoc_files)} entries)')

    # Classify everything up front.
    bucketed = {'cpp': [], 'cuda': [], 'vendored': [], 'skip': [], 'missing': []}
    for p in ad_hoc_files:
        if not p.exists():
            bucketed['missing'].append(p)
            continue
        bucketed[classify(p)].append(p)

    if list_mode:
        print(f'  cpp/h/hpp/inl     : {len(bucketed["cpp"])}')
        print(f'  cuda (.cu/.cuh)   : {len(bucketed["cuda"])}')
        print(f'  vendored (skip)   : {len(bucketed["vendored"])}')
        print(f'  non-cpp (skip)    : {len(bucketed["skip"])}')
        print(f'  missing on disk   : {len(bucketed["missing"])}')

    target_paths = bucketed['cpp'] + bucketed['cuda']

    # Pre-flight: per-file `git restore --` for the files we WILL format.
    if list_mode and not args.no_preflight_restore:
        preflight_git_restore(target_paths)

    counts = {'ok': 0, 'kept': 0, 'cuda': 0, 'html-skip': 0, 'missing': 0}
    regression_path = None

    try:
        for p in target_paths:
            skip_gpp = classify(p) == 'cuda'
            try:
                status = process_file(p, check=args.check, std=args.std, skip_gpp=skip_gpp)
            except Regression as reg:
                print('')
                print('=' * 80)
                print(f'[REGRESSION] {reg.path}')
                print(f'  baseline errors    : {reg.baseline}')
                print(f'  post-format errors : {reg.after}')
                print('  ----- g++ stderr (first 50 lines) -----')
                for ln in reg.stderr.splitlines()[:50]:
                    print(f'  | {ln}')
                print('=' * 80)
                print(f'  file left ON DISK for inspection: {reg.path}')
                print(f'  run: g++ -fsyntax-only -std={args.std} -w -x c++ {reg.path}')
                print(f'  diff: git diff -- {reg.path}')
                if not args.keep_going:
                    regression_path = reg.path
                    break
                regression_path = regression_path or reg.path
                continue
            counts[status] = counts.get(status, 0) + 1
    finally:
        print('')
        print('=' * 80)
        print('summary')
        print(f'  formatted ok          : {counts["ok"]}')
        print(f'  baseline errors kept  : {counts["kept"]}')
        print(f'  CUDA (format only)    : {counts["cuda"]}')
        print(f'  HTML refused          : {counts["html-skip"]}')
        if list_mode:
            print(f'  vendored skipped      : {len(bucketed["vendored"])}')
            print(f'  non-cpp skipped       : {len(bucketed["skip"])}')
            print(f'  missing on disk       : {len(bucketed["missing"])}')
        if regression_path is not None:
            print(f'  HALTED on regression  : {regression_path}')
        print('=' * 80)

    return 1 if regression_path is not None else 0


if __name__ == '__main__':
    sys.exit(main())
