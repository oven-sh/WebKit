#!/usr/bin/env python3
"""Writes runtime/PyCJKTables.inc: the character sets behind the Chinese, Japanese and Korean codecs (runtime/PyCJKCodecs.cpp).

    python3.15 tools/make_cjk_tables.py > runtime/PyCJKTables.inc

To be run with the CPython whose standard library is shipped. Nothing is read but what its codecs answer: every sequence of bytes is
decoded and every code point is encoded, and from that the tables of the character sets are put together, each of which serves
several codecs.

A table maps a code of two bytes, (lead << 8) | trail, to a character. What it is in C++:

    values      the characters, 16 bits each, row after row; 0xFFFF where there is none
    rows        for every lead byte from the first to the last: where its row starts in `values`, and the first and last trail byte
    plane2      if any character is beyond U+FFFF (all of those are in plane 2): a bit for each value
    overrides   the way back, from character to code, is made by turning the table round when it is first needed, the lowest
                code winning. These are the characters for which that is not what the codec does, with their codes, or 0xFFFF
                for a character that is not encoded at all.
"""
import sys

NONE = 0xFFFF
DEPENDS_ON_NEXT = 0xFFFE


def decode(codec, data):
    """The code points, or None."""
    try:
        return tuple(map(ord, bytes(data).decode(codec)))
    except UnicodeDecodeError:
        return None


def decode1(codec, data):
    result = decode(codec, data)
    return result[0] if result and len(result) == 1 else None


def encode(codec, *codepoints):
    try:
        return "".join(map(chr, codepoints)).encode(codec)
    except UnicodeEncodeError:
        return None


class Table:
    def __init__(self, name, to_character, to_code):
        self.name = name
        self.to_character = {code: c for code, c in to_character.items() if c is not None}
        self.to_code = to_code
        turned = {}
        for code in sorted(self.to_character):
            turned.setdefault(self.to_character[code], code)
        self.overrides = {c: to_code.get(c, NONE) for c in set(turned) | set(to_code) if turned.get(c) != to_code.get(c)}


GRID = [(a, b) for a in range(0x21, 0x7F) for b in range(0x21, 0x7F)]
BMP = range(0x80, 0x10000)


def jis_to_sjis(a, b):
    row, cell = a - 0x21, b - 0x21
    lead, trail = row >> 1, cell + (0x5E if row & 1 else 0)
    return lead + (0x81 if lead < 0x1F else 0xC1), trail + (0x40 if trail < 0x3F else 0x41)


def japanese():
    # JIS X 0208 and JIS X 0212, as EUC-JP has them. All codecs but the two Shift_JIS-2004 have U+FF3C at 0x2140 by a rule of
    # their own.
    jisx0208 = {a << 8 | b: decode1("euc_jp", (a | 0x80, b | 0x80)) for a, b in GRID}
    jisx0208[0x2140] = decode1("shift_jis_2004", jis_to_sjis(0x21, 0x40))
    jisx0212 = {a << 8 | b: decode1("euc_jp", (0x8F, a | 0x80, b | 0x80)) for a, b in GRID}
    to0208, to0212 = {}, {}
    for c in BMP:
        data = encode("euc_jp", c)
        if c == 0xFF3C or not data:
            continue
        if len(data) == 2 and data[0] != 0x8E:
            to0208[c] = (data[0] & 0x7F) << 8 | data[1] & 0x7F
        elif len(data) == 3:
            to0212[c] = (data[1] & 0x7F) << 8 | data[2] & 0x7F
    assert encode("shift_jis_2004", 0x5C) == bytes(jis_to_sjis(0x21, 0x40))
    to0208[0x5C] = 0x2140

    def common(c):
        """What the codecs fall back on: the code, and whether it is of JIS X 0212."""
        return (to0208[c], False) if c in to0208 else (to0212[c], True) if c in to0212 else None

    # JIS X 0213:2004. Plane 1 is JIS X 0208 and more: the table has what is more.
    plane1, plane2, pairs = {}, {}, {}
    for a, b in GRID:
        code = a << 8 | b
        characters = decode("shift_jis_2004", jis_to_sjis(a, b))
        if characters and len(characters) == 2:
            pairs[code] = characters
        elif characters and jisx0208[code] is None:
            plane1[code] = characters[0]
        plane2[code] = decode1("iso2022_jp_2004", (0x1B, 0x24, 0x28, 0x50, a, b))
    starters = {first for first, second in pairs.values()}
    to0213_1, to0213_2, alone = {}, {}, {}
    for c in list(range(0x80, 0x10000)) + list(range(0x20000, 0x30000)) + [0x7E]:
        data = encode("euc_jis_2004", c) if c != 0x7E else bytes(x | 0x80 for x in (0x22, 0x32)) if encode("shift_jis_2004", c) == bytes(jis_to_sjis(0x22, 0x32)) else None
        if not data or data[0] == 0x8E or c in (0xFF3C, 0xFF5E):
            continue
        code, second = ((data[1] & 0x7F) << 8 | data[2] & 0x7F, True) if len(data) == 3 else ((data[0] & 0x7F) << 8 | data[1] & 0x7F, False)
        if c in starters:
            assert not second
            alone[c] = code
        elif common(c) != (code, second):
            (to0213_2 if second else to0213_1)[c] = code
    for code, (first, second) in pairs.items():
        assert encode("euc_jis_2004", first, second) == bytes((code >> 8 | 0x80, code & 0xFF | 0x80))
    # In the table, those are marked.
    to0213_1.update(dict.fromkeys(starters, DEPENDS_ON_NEXT))

    # What Microsoft's code page 932 has over Shift_JIS, or has otherwise. In the bytes of Shift_JIS.
    cp932, to932 = {}, {}
    for lead in list(range(0x81, 0xA0)) + list(range(0xE0, 0xFD)):
        for trail in range(0x40, 0xFD):
            if trail == 0x7F:
                continue
            row, cell = (lead - (0x81 if lead < 0xE0 else 0xC1)) * 2, trail - (0x40 if trail < 0x80 else 0x41)
            a, b = row + (cell >= 0x5E) + 0x21, cell % 0x5E + 0x21
            if lead <= 0xEA:
                otherwise = jisx0208[a << 8 | b]
            elif 0xF0 <= lead <= 0xF9:
                otherwise = 0xE000 + 188 * (lead - 0xF0) + cell
            else:
                otherwise = None
            character = decode1("cp932", (lead, trail))
            if character != otherwise:
                assert character is not None
                cp932[lead << 8 | trail] = character
    for c in BMP:
        data = encode("cp932", c)
        if data and len(data) == 2:
            if c in to0208:
                otherwise = bytes(jis_to_sjis(to0208[c] >> 8, to0208[c] & 0xFF))
            elif 0xE000 <= c < 0xE758 and c not in to0212:
                cell = (c - 0xE000) % 188
                otherwise = bytes(((c - 0xE000) // 188 + 0xF0, cell + (0x40 if cell < 0x3F else 0x41)))
            else:
                otherwise = None
            if data != otherwise:
                to932[c] = data[0] << 8 | data[1]
        else:
            assert c not in to0208 or (data and len(data) == 1), hex(c)

    return [Table("jisx0208", jisx0208, to0208), Table("jisx0212", jisx0212, to0212), Table("jisx0213Plane1", plane1, to0213_1),
            Table("jisx0213Plane2", plane2, to0213_2), Table("cp932Extension", cp932, to932)], pairs, alone


def korean():
    ksx1001, extension, to1001, to_extension = {}, {}, {}, {}
    for lead in range(0x81, 0xFF):
        for trail in range(0x41, 0xFF):
            character = decode1("cp949", (lead, trail))
            if lead >= 0xA1 and trail >= 0xA1:
                ksx1001[(lead & 0x7F) << 8 | trail & 0x7F] = character
            else:
                extension[lead << 8 | trail] = character
    for c in BMP:
        data = encode("cp949", c)
        if data and data[0] >= 0xA1 and data[1] >= 0xA1:
            to1001[c] = (data[0] & 0x7F) << 8 | data[1] & 0x7F
        elif data:
            to_extension[c] = data[0] << 8 | data[1]
    return [Table("ksx1001", ksx1001, to1001), Table("cp949Extension", extension, to_extension)]


def chinese():
    gb2312 = {a << 8 | b: decode1("gb2312", (a | 0x80, b | 0x80)) for a, b in GRID}
    to2312 = {}
    for c in BMP:
        data = encode("gb2312", c)
        if data:
            to2312[c] = (data[0] & 0x7F) << 8 | data[1] & 0x7F
    # The three codes and four characters that GBK has otherwise than GB 2312 are a rule of the codec's.
    special_codes, special_characters = (0xA1AA, 0xA844, 0xA1A4), (0x2014, 0x2015, 0x00B7, 0x30FB)
    gbk, gb18030, to_gbk, to18030 = {}, {}, {}, {}
    for lead in range(0x81, 0xFF):
        for trail in range(0x40, 0xFF):
            code = lead << 8 | trail
            inner = gb2312.get((lead ^ 0x80) << 8 | trail ^ 0x80)
            character = decode1("gbk", (lead, trail))
            if code not in special_codes and inner is None:
                gbk[code] = character
            else:
                assert code in special_codes or character == inner
            wider = decode1("gb18030", (lead, trail))
            if character is None:
                gb18030[code] = wider
            else:
                assert wider == character
    ranges = []
    for c in BMP:
        data = encode("gbk", c)
        if data and c not in special_characters and c not in to2312:
            to_gbk[c] = data[0] << 8 | data[1]
        wider = encode("gb18030", c)
        if data:
            assert wider == data
        elif wider and len(wider) == 2:
            to18030[c] = wider[0] << 8 | wider[1]
        elif wider:
            linear = (((wider[0] - 0x81) * 10 + wider[1] - 0x30) * 126 + wider[2] - 0x81) * 10 + wider[3] - 0x30
            if ranges and ranges[-1][1] == c - 1 and ranges[-1][2] + c - ranges[-1][0] == linear:
                ranges[-1][1] = c
            else:
                ranges.append([c, c, linear])
    return [Table("gb2312", gb2312, to2312), Table("gbkExtension", gbk, to_gbk), Table("gb18030Extension", gb18030, to18030)], ranges


def taiwanese():
    big5, cp950, hkscs, to_big5, to950, to_hkscs = {}, {}, {}, {}, {}, {}
    for lead in range(0x81, 0xFF):
        for trail in range(0x40, 0xFF):
            code = lead << 8 | trail
            big5[code] = decode1("big5", (lead, trail))
            character = decode1("cp950", (lead, trail))
            if character != big5[code]:
                assert character is not None
                cp950[code] = character
            # Big5-HKSCS does not look at Big5 from 0xC6A1 to 0xC8FE.
            otherwise = big5[code] if lead < 0xC6 or lead > 0xC8 or (lead == 0xC6 and trail < 0xA1) else None
            character = decode1("big5hkscs", (lead, trail))
            if character != otherwise:
                assert otherwise is None
                hkscs[code] = character
    for c in list(BMP) + list(range(0x20000, 0x30000)):
        data = encode("big5", c)
        if data:
            to_big5[c] = data[0] << 8 | data[1]
        wider = encode("cp950", c)
        if wider != data:
            to950[c] = wider[0] << 8 | wider[1]
        wider = encode("big5hkscs", c)
        if wider != data and c not in (0xCA, 0xEA):
            to_hkscs[c] = wider[0] << 8 | wider[1]
    return [Table("big5", big5, to_big5), Table("cp950Extension", cp950, to950), Table("big5hkscsExtension", hkscs, to_hkscs)]


def numbers(values, per_line=24):
    lines = []
    for i in range(0, len(values), per_line):
        lines.append("    " + ",".join(str(v) for v in values[i:i + per_line]) + ",")
    return "\n".join(lines)


def emit(table):
    leads = sorted({code >> 8 for code in table.to_character})
    first, last = leads[0], leads[-1]
    values, rows, wide = [], [], []
    for lead in range(first, last + 1):
        trails = sorted(code & 0xFF for code in table.to_character if code >> 8 == lead)
        if not trails:
            rows.append((0, 1, 0))
            continue
        rows.append((len(values), trails[0], trails[-1]))
        for trail in range(trails[0], trails[-1] + 1):
            character = table.to_character.get(lead << 8 | trail, NONE)
            assert character < 0x10000 or character >> 16 == 2
            assert character & 0xFFFF != NONE or character == NONE
            values.append(character & 0xFFFF)
            wide.append(character >= 0x10000)
    assert len(values) < 0x10000
    name = table.name
    print("static constexpr uint16_t %sValues[] = {\n%s\n};" % (name, numbers(values)))
    print("static constexpr Row %sRows[] = {\n%s\n};" % (name, "\n".join("    " + " ".join("{ %d, %d, %d }," % row for row in rows[i:i + 8]) for i in range(0, len(rows), 8))))
    size = 2 * len(values) + 4 * len(rows)
    if any(wide):
        bits = [sum(1 << j for j in range(8) if i + j < len(wide) and wide[i + j]) for i in range(0, len(wide), 8)]
        print("static constexpr uint8_t %sPlane2[] = {\n%s\n};" % (name, numbers(bits, 32)))
        size += len(bits)
    parts = []
    for suffix, chosen in (("Overrides", [c for c in sorted(table.overrides) if c < 0x10000]), ("Plane2Overrides", [c for c in sorted(table.overrides) if c >= 0x10000])):
        if chosen:
            print("static constexpr Override %s%s[] = {\n%s\n};" % (name, suffix, "\n".join("    " + " ".join("{ %d, %d }," % (c & 0xFFFF, table.overrides[c]) for c in chosen[i:i + 10]) for i in range(0, len(chosen), 10))))
            parts.append("%s%s" % (name, suffix))
            size += 4 * len(chosen)
        else:
            parts.append("{ }")
    print("static constexpr TableData %sData { %d, %d, %sRows, %sValues, %s, %s, %s };\n" % (name, first, last, name, name, name + "Plane2" if any(wide) else "nullptr", parts[0], parts[1]))
    print("// %-20s %5d characters, %5d values, %4d overrides, %6d bytes" % (name, len(table.to_character), len(values), len(table.overrides), size), file=sys.stderr)
    return size


def main():
    print("// Generated by tools/make_cjk_tables.py from the codecs of CPython %d.%d.%d. Do not edit.\n" % sys.version_info[:3])
    japan, pairs, alone = japanese()
    china, ranges = chinese()
    total = sum(emit(table) for table in japan + korean() + china + taiwanese())

    print("// The codes of JIS X 0213 that stand for two characters, by the characters.")
    print("static constexpr Pair jisx0213Pairs[] = {")
    for code, (first, second) in sorted(pairs.items(), key=lambda item: item[1]):
        print("    { %d, %d, %d }," % (first, second, code))
    print("};")
    print("// The first of such characters, with the code each has by itself.")
    print("static constexpr Override jisx0213PairStarters[] = {\n    %s\n};\n" % " ".join("{ %d, %d }," % item for item in sorted(alone.items())))
    print("// The characters of the BMP that GB 18030 writes with four bytes: first, last, and the number of the first among all sequences of four bytes.")
    print("static constexpr Range gb18030Ranges[] = {\n%s\n};" % "\n".join("    " + " ".join("{ %d, %d, %d }," % tuple(r) for r in ranges[i:i + 6]) for i in range(0, len(ranges), 6)))
    # The decoder finds a range by the number alone.
    assert ranges[0][2] == 0 and all(a[2] + a[1] - a[0] + 1 == b[2] for a, b in zip(ranges, ranges[1:]))
    print("static constexpr unsigned gb18030RangesEnd = %d;\n" % (ranges[-1][2] + ranges[-1][1] - ranges[-1][0] + 1))
    print("// ISO 8859-7 as ISO-2022-JP-2 has it in G2: what the byte after ESC N stands for.")
    print("static constexpr uint16_t iso2022Greek[256] = {\n%s\n};" % numbers([decode1("iso2022_jp_2", (0x1B, 0x2E, 0x46, 0x1B, 0x4E, byte)) or (NONE if byte != 0x80 else 0) for byte in range(256)]))
    total += 6 * len(pairs) + 4 * len(alone) + 6 * len(ranges) + 512
    print("// %d pairs, %d ranges; %d bytes in all" % (len(pairs), len(ranges), total), file=sys.stderr)


main()
