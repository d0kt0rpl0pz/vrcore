#!/usr/bin/env python3
"""textstrings.py -- turns a textstrings.csv into a C++ header of string constants.

The headset's UI wording lives in a CSV (columns id,en,note) so there is one easy place to edit it. CMake runs
this at build time (vrcore_textstrings() in CMakeLists.txt), so nothing is parsed at run time, there is no
missing-file case, and a wrong ID is a compile error.

  textstrings.py <textstrings.csv> <out.h> <namespace>

The header holds one `inline constexpr const char *` per row, in <namespace>::txt, named by the id with its dots
turned into underscores (soundtrack.shuffle.help -> txt::soundtrack_shuffle_help). The text is kept as written,
so `{slot}` stays for the fmt() helper to fill and `{{` / `}}` are literal braces.

Every check below stops the build (exit 1, the message names the file and line):
  - a header row other than id,en,note, or a row with the wrong number of fields
  - an id that is not [a-z][a-z0-9_]*(.[a-z0-9_]+)* (a leading digit would not be a C++ name), a duplicate id,
    two ids equal once dots become underscores, an id whose name is a C++ keyword (default, new, ...), or one that
    makes a `__` name (reserved in C++)
  - an empty en
  - a {slot} that is not [a-z_]+, or an unbalanced brace (a literal brace is written {{ or }})
  - a character in id or en outside printable ASCII 0x20-0x7E, except a line break inside en: the menu fonts are
    baked with ImGui's default (basic Latin) range, so a curly quote or a dash pasted from Numbers would show as a
    box in the headset, and a tab or other control character would not draw at all

A line break inside a quoted en field is a line break in the text: CRLF and a lone CR are folded to LF first (so
a file saved by Numbers or Excel reads the same), and the header holds it as \n.

Parsing is RFC 4180 (quoted fields, "" inside quotes, quoted newlines) and tolerates a BOM and CRLF. A line that
starts with # (outside a quoted field) is a comment and is skipped before the CSV is parsed, so a stray quote in a
comment cannot swallow the rows after it. A row whose fields are all empty (Numbers writes them as ,,) is skipped.
The header is written only when its contents change (through a temporary file, so an interrupted build never
leaves half a header that looks up to date); when they are equal the file is left alone, and the build
(restat) recompiles nothing that includes it. The command itself runs again on each build while the CSV is
newer than the header (a save with no change).
"""

import csv
import os
import re
import sys

COLUMNS = ["id", "en", "note"]
ID_RE = re.compile(r"[a-z][a-z0-9_]*(\.[a-z0-9_]+)*\Z")
NAMESPACE_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*(::[A-Za-z_][A-Za-z0-9_]*)*\Z")
SLOT_RE = re.compile(r"[a-z_]+\Z")
# The C++17 keywords and alternative tokens that a one-word id could hit (an id with a dot never can).
KEYWORDS = set("""
    alignas alignof and and_eq asm auto bitand bitor bool break case catch char char16_t char32_t class compl
    const const_cast constexpr continue decltype default delete do double dynamic_cast else enum explicit export
    extern false float for friend goto if inline int long mutable namespace new noexcept not not_eq nullptr
    operator or or_eq private protected public register reinterpret_cast return short signed sizeof static
    static_assert static_cast struct switch template this thread_local throw true try typedef typeid typename
    union unsigned using virtual void volatile wchar_t while xor xor_eq
""".split())


class Bad(Exception):
    pass


def check_text(text):
    """The brace and glyph rules for one en string; raises Bad."""
    for ch in text:
        if ch != "\n" and not 0x20 <= ord(ch) <= 0x7E:
            raise Bad(f"character U+{ord(ch):04X} is outside printable ASCII (the menu fonts have basic Latin only)")
    i = 0
    while i < len(text):
        ch = text[i]
        if ch == "{":
            if text.startswith("{{", i):
                i += 2
                continue
            end = text.find("}", i + 1)
            if end < 0:
                raise Bad("unbalanced '{' (write a literal brace as '{{')")
            slot = text[i + 1:end]
            if not SLOT_RE.match(slot):
                raise Bad(f"slot '{{{slot}}}' must be lower case letters and underscores, as {{name}}")
            i = end + 1
        elif ch == "}":
            if text.startswith("}}", i):
                i += 2
                continue
            raise Bad("unbalanced '}' (write a literal brace as '}}')")
        else:
            i += 1


def uncommented(f):
    """The file's lines, with each comment line (# first, outside a quoted field) emptied so line numbers stay."""
    quoted = False
    for line in f:
        if not quoted and line.startswith("#"):
            yield "\n"
            continue
        yield line
        if line.count('"') % 2:
            quoted = not quoted


def read_rows(path):
    """The (line, id, en) of each string row, checked; raises Bad with 'file:line: message'."""
    try:
        f = open(path, newline="", encoding="utf-8-sig")
    except OSError as e:
        raise Bad(f"{path}: {e.strerror}")
    rows, seen, flat = [], {}, {}
    with f:
        reader = csv.reader(uncommented(f), strict=True)
        header = False
        try:
            for fields in reader:
                line = reader.line_num
                where = f"{path}:{line}"
                if not any(field.strip() for field in fields):
                    continue                                  # a blank line, or a comment emptied above
                if not header:
                    if fields != COLUMNS:
                        raise Bad(f"{where}: the header row must be {','.join(COLUMNS)}")
                    header = True
                    continue
                if len(fields) != len(COLUMNS):
                    raise Bad(f"{where}: {len(fields)} fields, expected {len(COLUMNS)} ({','.join(COLUMNS)})")
                rid, en = fields[0], fields[1].replace("\r\n", "\n").replace("\r", "\n")
                if not ID_RE.match(rid):
                    raise Bad(f"{where}: id '{rid}' must be lower case words joined by dots, starting with a letter, "
                              "as soundtrack.shuffle.help")
                if rid in seen:
                    raise Bad(f"{where}: duplicate id '{rid}' (first at line {seen[rid]})")
                name = rid.replace(".", "_")
                if "__" in name:
                    raise Bad(f"{where}: id '{rid}' makes a name with a double underscore, which C++ reserves")
                if name in KEYWORDS:
                    raise Bad(f"{where}: id '{rid}' is a C++ keyword; give it a dotted prefix, as menu.{rid}")
                if name in flat:
                    raise Bad(f"{where}: id '{rid}' and '{flat[name]}' are both {name} "
                              "once the dots become underscores")
                if en == "":
                    raise Bad(f"{where}: '{rid}' has an empty en")
                try:
                    check_text(en)
                except Bad as e:
                    raise Bad(f"{where}: '{rid}': {e}")
                seen[rid] = line
                flat[name] = rid
                rows.append((name, en))
        except csv.Error as e:
            raise Bad(f"{path}:{reader.line_num}: {e}")
        except UnicodeDecodeError as e:
            raise Bad(f"{path}: not UTF-8 ({e.reason})")
        if not header:
            raise Bad(f"{path}: no header row (id,en,note)")
    return rows


def c_string(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def render(rows, namespace, csv_name):
    out = [
        f"// Generated by tools/textstrings.py from {csv_name}. Do not edit: change the CSV.",
        "// The headset UI wording as constants; {slot} is filled by fmt(), {{ and }} are literal braces.",
        "#pragma once",
        "",
        f"namespace {namespace} {{",
        "namespace txt {",
        "",
    ]
    out += [f"inline constexpr const char *{name} = {c_string(en)};" for name, en in rows]
    out += ["", "}  // namespace txt", f"}}  // namespace {namespace}", ""]
    return "\n".join(out)


def main(argv):
    if len(argv) != 4:
        print("usage: textstrings.py <textstrings.csv> <out.h> <namespace>", file=sys.stderr)
        return 2
    src, dst, namespace = argv[1:]
    try:
        if not NAMESPACE_RE.match(namespace):
            raise Bad(f"namespace '{namespace}' is not a C++ namespace name")
        text = render(read_rows(src), namespace, os.path.basename(src))
    except Bad as e:
        print(f"textstrings: {e}", file=sys.stderr)
        return 1
    try:
        with open(dst, encoding="utf-8", newline="") as f:
            if f.read() == text:
                return 0
    except OSError:
        pass
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    tmp = dst + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    os.replace(tmp, dst)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
