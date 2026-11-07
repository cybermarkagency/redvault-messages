"""Converts WOFF (1.0) fonts to plain TrueType/OpenType, which every font
backend Qt uses can load (DirectWrite on Windows can't read WOFF).

    python desktop/tools/woff_to_ttf.py in.woff [more.woff ...]

Writes each next to its input with a .ttf extension. WOFF 1.0 is just the
sfnt tables, each optionally zlib-compressed, behind a small header.

Web font subsets often name each weight as a family of its own ("IBM Plex
Sans SemiBold", style "Regular"). Those are renamed to the base family with
the weight as the style, so Qt finds them by weight (Font.DemiBold, ...).
"""
import struct
import sys
import zlib
from pathlib import Path


WEIGHT_WORDS = ("ExtraLight", "Light", "Thin", "Medium", "SemiBold", "ExtraBold", "Black")


def merge_family(name_table: bytes) -> bytes:
    """Moves a weight word from the family name (id 1) into the style (id 2)."""
    fmt, count, string_offset = struct.unpack(">HHH", name_table[:6])
    records = []
    for i in range(count):
        pid, eid, lid, nid, length, offset = struct.unpack(">HHHHHH", name_table[6 + i * 12 : 18 + i * 12])
        raw = name_table[string_offset + offset : string_offset + offset + length]
        records.append([pid, eid, lid, nid, raw])
    encoding = lambda pid: "utf-16-be" if pid in (0, 3) else "latin-1"
    weight = None
    for r in records:
        if r[3] == 1:
            family = r[4].decode(encoding(r[0]))
            for word in WEIGHT_WORDS:
                if family.endswith(" " + word):
                    weight = word
                    r[4] = family[: -len(word) - 1].encode(encoding(r[0]))
    if weight is None:
        return name_table
    for r in records:
        if r[3] == 2:
            r[4] = weight.encode(encoding(r[0]))
    header = struct.pack(">HHH", 0, len(records), 6 + len(records) * 12)
    directory, strings = bytearray(), bytearray()
    for pid, eid, lid, nid, raw in records:
        directory += struct.pack(">HHHHHH", pid, eid, lid, nid, len(raw), len(strings))
        strings += raw
    return header + bytes(directory) + bytes(strings)


def convert(data: bytes) -> bytes:
    signature, flavor, _length, num_tables = struct.unpack(">4sIIH", data[:14])
    if signature != b"wOFF":
        raise ValueError("not a WOFF 1.0 file")
    tables = []
    for i in range(num_tables):
        tag, offset, comp_len, orig_len, checksum = struct.unpack(">4sIIII", data[44 + i * 20 : 64 + i * 20])
        raw = data[offset : offset + comp_len]
        table = zlib.decompress(raw) if comp_len < orig_len else raw
        if len(table) != orig_len:
            raise ValueError(f"table {tag!r}: bad length")
        if tag == b"name":
            table = merge_family(table)
            checksum = sum(struct.unpack(f">{(len(table) + 3) // 4}I", table + bytes(-len(table) % 4))) & 0xFFFFFFFF
        tables.append((tag, checksum, table))

    # sfnt header: search range fields from the table count.
    entry_selector = max(num_tables.bit_length() - 1, 0)
    search_range = (1 << entry_selector) * 16
    range_shift = num_tables * 16 - search_range
    out = bytearray(struct.pack(">IHHHH", flavor, num_tables, search_range, entry_selector, range_shift))
    offset = 12 + num_tables * 16
    directory = bytearray()
    body = bytearray()
    for tag, checksum, table in sorted(tables, key=lambda t: t[0]):
        directory += struct.pack(">4sIII", tag, checksum, offset + len(body), len(table))
        body += table
        body += b"\0" * (-len(table) % 4)  # tables are 4-byte aligned
    return bytes(out + directory + body)


def main() -> None:
    for name in sys.argv[1:]:
        path = Path(name)
        path.with_suffix(".ttf").write_bytes(convert(path.read_bytes()))
        print(path.with_suffix(".ttf").name)


if __name__ == "__main__":
    main()
