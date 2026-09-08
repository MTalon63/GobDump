#!/usr/bin/env python3
"""Rebrand i18n files from satdump to gobdump: update msgid/msgstr lines and recompile .mo."""
import ast
import os
import struct
import sys

BASE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "resources", "i18n")

PO_FILES = [
    "po/gobdump.pot",
    "po/fr/gobdump.po",
    "po/it/gobdump.po",
    "po/pt-br/gobdump.po",
    "po/sk/gobdump.po",
]

MO_OUTPUTS = [
    ("po/fr/gobdump.po", "po/fr/gobdump.mo"),
    ("po/it/gobdump.po", "po/it/gobdump.mo"),
    ("po/pt-br/gobdump.po", "po/pt-br/gobdump.mo"),
    ("po/sk/gobdump.po", "po/sk/gobdump.mo"),
    ("po/fr/gobdump.po", "fr/LC_MESSAGES/gobdump.mo"),
    ("po/it/gobdump.po", "it/LC_MESSAGES/gobdump.mo"),
    ("po/pt-br/gobdump.po", "pt-br/LC_MESSAGES/gobdump.mo"),
    ("po/sk/gobdump.po", "sk/LC_MESSAGES/gobdump.mo"),
]


def update_po(path):
    with open(path, "r", encoding="utf-8") as f:
        lines = f.readlines()
    changed = 0
    for i, line in enumerate(lines):
        if line.startswith("msgid "):
            new = line.replace("SatDump", "GobDump")
            if new != line:
                lines[i] = new
                changed += 1
        elif line.startswith("msgstr "):
            new = line.replace("SatDump", "GobDump").replace("Satdump", "GobDump")
            if new != line:
                lines[i] = new
                changed += 1
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.writelines(lines)
    print(f"  {path}: {changed} line(s) updated")
    return changed


def parse_po(path):
    """Parse a .po file into a list of (msgid, msgstr) tuples, handling multiline strings."""
    with open(path, "r", encoding="utf-8") as f:
        content = f.read()
    entries = []
    # Split on blank lines separating entries
    blocks = content.split("\n\n")
    for block in blocks:
        if not block.strip():
            continue
        msgid = None
        msgstr = None
        in_msgid = False
        in_msgstr = False
        for line in block.split("\n"):
            if line.startswith("msgid "):
                msgid = _unescape(line[len("msgid "):].strip())
                in_msgid = True
                in_msgstr = False
            elif line.startswith("msgstr "):
                msgstr = _unescape(line[len("msgstr "):].strip())
                in_msgid = False
                in_msgstr = True
            elif line.startswith('"') and in_msgid:
                msgid += _unescape(line.strip())
            elif line.startswith('"') and in_msgstr:
                msgstr += _unescape(line.strip())
        if msgid is not None:
            entries.append((msgid, msgstr if msgstr is not None else ""))
    return entries


def _unescape(s):
    """Unescape a quoted gettext string literal (e.g. \"...\") to its raw value."""
    try:
        return ast.literal_eval(s)
    except Exception:
        return s.strip('"')


def compile_mo(po_path, mo_path):
    entries = parse_po(po_path)
    # Sort by msgid (byte order)
    entries.sort(key=lambda e: e[0].encode("utf-8"))
    n = len(entries)
    # Header: magic, version, count, orig table offset, trans table offset, hash size, hash offset
    orig_table_off = 28
    trans_table_off = orig_table_off + 8 * n
    hash_table_off = trans_table_off + 8 * n
    str_table_off = hash_table_off + 4 * n
    header = struct.pack(
        "<Iiiiiii",
        0x950412DE,
        0,
        n,
        orig_table_off,
        trans_table_off,
        0,
        hash_table_off,
    )
    # Build the string table; offsets are absolute file offsets (str_table_off + position)
    offsets = []
    str_table = b""
    for msgid, msgstr in entries:
        mid = msgid.encode("utf-8")
        mst = msgstr.encode("utf-8")
        moff = str_table_off + len(str_table)
        soff = moff + len(mid) + 1
        offsets.append((len(mid), len(mst), moff, soff))
        str_table += mid + b"\x00" + mst + b"\x00"
    orig_table = b""
    trans_table = b""
    for (mlen, slen, moff, soff) in offsets:
        orig_table += struct.pack("<II", mlen, moff)
        trans_table += struct.pack("<II", slen, soff)
    hash_table = b"\x00" * (4 * n)
    data = header + orig_table + trans_table + hash_table + str_table
    os.makedirs(os.path.dirname(mo_path), exist_ok=True)
    with open(mo_path, "wb") as f:
        f.write(data)
    print(f"  {mo_path}: compiled {n} entries")


def verify_mo(mo_path, lang):
    import gettext
    try:
        with open(mo_path, "rb") as f:
            t = gettext.GNUTranslations(f)
        # Check a known msgid resolves
        for key in ["GobDump", "Welcome to GobDump!", "GobDump Log"]:
            val = t.gettext(key)
            if val:
                print(f"    [{lang}] '{key}' -> '{val}'")
        return True
    except Exception as e:
        print(f"    [{lang}] VERIFY FAILED: {e}")
        return False


def main():
    print("Updating .po/.pot msgid/msgstr lines...")
    for p in PO_FILES:
        update_po(os.path.join(BASE, p))

    print("Compiling .mo files...")
    for po_rel, mo_rel in MO_OUTPUTS:
        compile_mo(os.path.join(BASE, po_rel), os.path.join(BASE, mo_rel))

    print("Verifying .mo files with gettext...")
    for po_rel, mo_rel in MO_OUTPUTS:
        if "LC_MESSAGES" in mo_rel:
            lang = mo_rel.split("/")[0]
            verify_mo(os.path.join(BASE, mo_rel), lang)


if __name__ == "__main__":
    main()