#!/usr/bin/env python3
"""textstrings_check.py -- lists the headset-looking string literals still left in C++ code.

The headset's wording belongs in textstrings.csv (tools/textstrings.py turns it into constants). This finds the
literals that were missed, so a new one added in code shows up before it ships:

  textstrings_check.py [--allow FILE] [DIR ...]       (default: host/ next to this script's folder)

A literal looks like headset text when it starts with a capital letter followed by a lower case one ("Off",
"Resume", "Now playing: ") or is two or more words of letters ("the file"). Skipped before looking: comments,
#include lines, and lines that are logging or test output (log(, printf, r.check(, SDL_Log, and the like).

The allowlist (tools/textstrings_allow.txt) says what may stay, one rule per line, `#` starts a comment:
  file <glob>                  the whole file is skipped (self-tests, probes, the test fronts)
  literal <path-glob> <text>   this exact literal (the text after the second space, as it appears between quotes)
                               may stay in the files matching the glob

Exit 0 when nothing is left, 1 when something is (each printed as path:line: "literal"), 2 on a bad allowlist.
"""

import fnmatch
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXTS = (".cpp", ".h", ".c", ".mm")
LITERAL_RE = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
# a line that is logging or test output: its strings are not shown in the headset
QUIET_RE = re.compile(r"\b(log|printf|fprintf|snprintf_|SDL_Log\w*|r\.check|check|fail|expect|assert|"
                      r"std::cerr|std::cout|puts)\s*\(|\bLOG_\w+\(|^\s*#\s*(include|error|pragma|define\s+\w+_H\b)")
WORDY_RE = re.compile(r"^[A-Z][a-z]|^[A-Za-z]+( [A-Za-z]+)+")


class BadAllow(Exception):
    pass


def read_allow(path):
    files, literals = [], []
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = line.split(" ", 2)
            if parts[0] == "file" and len(parts) == 2:
                files.append(parts[1])
            elif parts[0] == "literal" and len(parts) == 3 and parts[2]:
                literals.append((parts[1], parts[2]))
            else:
                raise BadAllow(f"{path}:{n}: expected 'file <glob>' or 'literal <path-glob> <text>'")
    return files, literals


def strip_comments(lines):
    """Each line with comments and char literals blanked, string literals kept; block comments span lines."""
    in_block = False
    for line in lines:
        out, i, n = [], 0, len(line)
        while i < n:
            if in_block:
                end = line.find("*/", i)
                if end < 0:
                    i = n
                else:
                    in_block, i = False, end + 2
                continue
            two = line[i:i + 2]
            if two == "/*":
                in_block, i = True, i + 2
            elif two == "//":
                break
            elif line[i] == '"':
                m = LITERAL_RE.match(line, i)
                end = m.end() if m else n
                out.append(line[i:end])
                i = end
            elif line[i] == "'":
                m = re.compile(r"'(?:[^'\\]|\\.)*'").match(line, i)
                i = m.end() if m else i + 1
                out.append(" ")
            else:
                out.append(line[i])
                i += 1
        yield "".join(out)


def scan(path, rel, literals):
    found = []
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.read().split("\n")
    for n, line in enumerate(strip_comments(lines), 1):
        if QUIET_RE.search(line):
            continue
        for m in LITERAL_RE.finditer(line):
            text = m.group(1)
            if not WORDY_RE.match(text):
                continue
            if any(fnmatch.fnmatch(rel, g) and text == t for g, t in literals):
                continue
            found.append((n, text))
    return found


def main(argv):
    allow = os.path.join(HERE, "textstrings_allow.txt")
    dirs, args = [], argv[1:]
    while args:
        a = args.pop(0)
        if a == "--allow" and args:
            allow = args.pop(0)
        else:
            dirs.append(a)
    if not dirs:
        dirs = [os.path.join(ROOT, "host")]
    try:
        skip_files, literals = read_allow(allow) if os.path.exists(allow) else ([], [])
    except (BadAllow, OSError) as e:
        print(f"textstrings_check: {e}", file=sys.stderr)
        return 2
    total = 0
    for d in dirs:
        for dp, dn, fn in os.walk(d):
            dn.sort()
            for name in sorted(fn):
                if not name.endswith(EXTS):
                    continue
                path = os.path.join(dp, name)
                rel = os.path.relpath(path, ROOT).replace(os.sep, "/")
                if any(fnmatch.fnmatch(rel, g) for g in skip_files):
                    continue
                for n, text in scan(path, rel, literals):
                    print(f'{rel}:{n}: "{text}"')
                    total += 1
    if total:
        print(f"textstrings_check: {total} literal(s) left; move them to the CSV or add them to {os.path.basename(allow)}",
              file=sys.stderr)
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
