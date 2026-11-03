#!/usr/bin/env python3
# Copyright (C) 2026 Oven, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-2-Clause

"""Writes parser/PyUnicodeTables.h from the Unicode Character Database.

    make_unicode_tables.py DIRECTORY [--check]

DIRECTORY holds these files of the version of Unicode that the version of Python we implement uses
(https://www.unicode.org/Public/<version>/ucd/): UnicodeData.txt, SpecialCasing.txt, CaseFolding.txt,
DerivedCoreProperties.txt, LineBreak.txt and extracted/DerivedNumericType.txt (here without "extracted/").

What the methods of str say about a character is part of the language, so it cannot depend on which ICU there is.

With --check, and run by a CPython with the same version of Unicode, every character is also compared with what that says.
"""

import os
import sys

FLAGS = [
    "IsAlpha", "IsLowercase", "IsUppercase", "IsTitlecase", "IsCased", "IsCaseIgnorable", "IsDecimal", "IsDigit",
    "IsNumeric", "IsWhitespace", "IsLineBreak", "IsPrintable", "IsXIDStart", "IsXIDContinue", "HasSequences",
]
for position, name in enumerate(FLAGS):
    globals()[name] = 1 << position

LIMIT = 0x110000


def lines(directory, name):
    with open(os.path.join(directory, name), encoding="utf-8") as file:
        for line in file:
            line = line.split("#", 1)[0].strip()
            if line:
                yield [field.strip() for field in line.split(";")]


def code_points(field):
    first, _, last = field.partition("..")
    return range(int(first, 16), int(last or first, 16) + 1)


def version_of(directory):
    with open(os.path.join(directory, "DerivedCoreProperties.txt"), encoding="utf-8") as file:
        return file.readline().strip().removeprefix("# DerivedCoreProperties-").removesuffix(".txt")


def read(directory):
    """For each character that is assigned: (flags, decimal, lower, upper, title, fold), the last four being lists."""
    data = {}
    first_of_range = None
    for fields in lines(directory, "UnicodeData.txt"):
        code = int(fields[0], 16)
        if fields[1].endswith(", First>"):
            first_of_range = code
            continue
        for each in range(first_of_range if fields[1].endswith(", Last>") else code, code + 1):
            data[each] = fields
        first_of_range = None

    properties = {}
    wanted = {
        "Lowercase": IsLowercase, "Uppercase": IsUppercase, "Cased": IsCased, "Case_Ignorable": IsCaseIgnorable,
        "XID_Start": IsXIDStart, "XID_Continue": IsXIDContinue,
    }
    for fields in lines(directory, "DerivedCoreProperties.txt"):
        if flag := wanted.get(fields[1]):
            for code in code_points(fields[0]):
                properties[code] = properties.get(code, 0) | flag
    for fields in lines(directory, "LineBreak.txt"):
        if fields[1] in ("BK", "CR", "LF", "NL"):
            for code in code_points(fields[0]):
                properties[code] = properties.get(code, 0) | IsLineBreak
    numeric = {"Decimal": IsDecimal | IsDigit | IsNumeric, "Digit": IsDigit | IsNumeric, "Numeric": IsNumeric}
    for fields in lines(directory, "DerivedNumericType.txt"):
        for code in code_points(fields[0]):
            properties[code] = properties.get(code, 0) | numeric[fields[1]]

    # Those that depend on the language or on what is around are left out; str.lower() knows about the final sigma.
    special = {}
    for fields in lines(directory, "SpecialCasing.txt"):
        if len(fields) == 5:
            special[int(fields[0], 16)] = [[int(c, 16) for c in field.split()] for field in fields[1:4]]
    folding = {}
    for fields in lines(directory, "CaseFolding.txt"):
        if fields[1] in ("C", "F"):
            folding[int(fields[0], 16)] = [int(c, 16) for c in fields[2].split()]

    result = {}
    for code, fields in data.items():
        category, bidirectional = fields[2], fields[4]
        flags = properties.get(code, 0)
        if category in ("Lm", "Lt", "Lu", "Ll", "Lo"):
            flags |= IsAlpha
        if category == "Lt":
            flags |= IsTitlecase
        if bidirectional == "B":
            flags |= IsLineBreak
        if category == "Zs" or bidirectional in ("WS", "B", "S"):
            flags |= IsWhitespace
        if code == 0x20 or category[0] not in "CZ":
            flags |= IsPrintable
        upper = [int(fields[12], 16)] if fields[12] else [code]
        lower = [int(fields[13], 16)] if fields[13] else [code]
        title = [int(fields[14], 16)] if fields[14] else upper
        if code in special:
            lower, title, upper = special[code]
        fold = folding.get(code, [code])
        decimal = int(fields[6]) if fields[6] else 0
        result[code] = (flags, decimal, lower, upper, title, fold)
    return result


def check(characters, version):
    import unicodedata
    if unicodedata.unidata_version != version:
        sys.exit(f"this Python has Unicode {unicodedata.unidata_version}, the files are {version}")
    nothing = (0, 0, None, None, None, None)
    wrong = 0
    for code in range(LIMIT):
        c = chr(code)
        flags, decimal, lower, upper, title, fold = characters.get(code, nothing)
        text = lambda mapping: c if mapping is None else "".join(map(chr, mapping))
        expected = {
            "isalpha": bool(flags & IsAlpha), "isdecimal": bool(flags & IsDecimal), "isdigit": bool(flags & IsDigit),
            "isnumeric": bool(flags & IsNumeric), "isspace": bool(flags & IsWhitespace),
            "isprintable": bool(flags & IsPrintable), "islower": bool(flags & IsLowercase),
            "isupper": bool(flags & IsUppercase), "istitle": bool(flags & (IsUppercase | IsTitlecase)),
            "isidentifier": bool(flags & IsXIDStart) or c == "_",
            "upper": text(upper), "title": text(title), "casefold": text(fold),
            "lower": text(lower) if code != 0x3a3 else "σ",
        }
        for name, value in expected.items():
            if getattr(c, name)() != value:
                wrong += 1
                print(f"U+{code:04X} {name}: {getattr(c, name)()!r}, not {value!r}")
        if ("a" + c).isidentifier() != bool(flags & IsXIDContinue):
            wrong += 1
            print(f"U+{code:04X} XID_Continue")
        if (len(("a" + c + "b").splitlines()) == 2) != bool(flags & IsLineBreak):
            wrong += 1
            print(f"U+{code:04X} line break")
        if flags & IsDecimal and int(c) != decimal:
            wrong += 1
            print(f"U+{code:04X} decimal")
        # A capital sigma is final after something cased, with what is ignorable skipped.
        if ("A" + c + "Σ").lower()[-1] != ("ς" if flags & (IsCased | IsCaseIgnorable) else "σ"):
            wrong += 1
            print(f"U+{code:04X} cased or ignorable")
        if ("AΣ" + c).lower()[1] != ("σ" if flags & IsCased and not flags & IsCaseIgnorable else "ς"):
            wrong += 1
            print(f"U+{code:04X} cased and not ignorable")
        if (c + "a").title()[-1] != ("a" if flags & IsCased else "A"):
            wrong += 1
            print(f"U+{code:04X} cased")
    print(f"checked against Python {sys.version.split()[0]}: {wrong} differences")
    if wrong:
        sys.exit(1)


def split(values):
    """The two stages that take the least room: (shift, stage1, stage2)."""
    best = None
    for shift in range(4, 11):
        size = 1 << shift
        blocks, stage1, stage2 = {}, [], []
        for start in range(0, len(values), size):
            block = tuple(values[start:start + size])
            if block not in blocks:
                blocks[block] = len(blocks)
                stage2.extend(block)
            stage1.append(blocks[block])
        cost = len(stage1) * width(stage1) + len(stage2) * width(stage2)
        if best is None or cost < best[0]:
            best = (cost, shift, stage1, stage2)
    return best[1:]


def width(values):
    return 1 if max(values) < 256 else 2


def array(out, name, values):
    out.append(f"static constexpr uint{8 * width(values)}_t {name}[] = {{")
    for start in range(0, len(values), 24):
        out.append("    " + " ".join(f"{value}," for value in values[start:start + 24]))
    out.append("};")
    out.append("")


def main():
    arguments = [argument for argument in sys.argv[1:] if not argument.startswith("--")]
    if len(arguments) != 1:
        sys.exit(__doc__)
    directory = arguments[0]
    version = version_of(directory)
    characters = read(directory)
    if "--check" in sys.argv:
        check(characters, version)

    records = {(0, 0, 0, 0, 0, 0): 0}
    sequences = []
    positions = {}

    def sequence(mapping):
        key = tuple(mapping)
        if key not in positions:
            positions[key] = len(sequences)
            sequences.extend(mapping)
        return positions[key] << 2 | len(mapping)

    indices = [0] * LIMIT
    for code, (flags, decimal, *mappings) in characters.items():
        if all(len(mapping) == 1 for mapping in mappings):
            mappings = [mapping[0] - code for mapping in mappings]
        else:
            flags |= HasSequences
            mappings = [sequence(mapping) for mapping in mappings]
        indices[code] = records.setdefault((flags, decimal, *mappings), len(records))
    shift, stage1, stage2 = split(indices)

    out = [
        "/*",
        " * Copyright (C) 2026 Oven, Inc. All rights reserved.",
        " * SPDX-License-Identifier: BSD-2-Clause",
        " */",
        "",
        f"// Made by tools/make_unicode_tables.py from the Unicode Character Database {version}. Not to be edited.",
        "",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace JSC {",
        "namespace Python {",
        "namespace UnicodeTables {",
        "",
        f'static constexpr const char* version = "{version}";',
        "",
        "enum Flag : uint16_t {",
    ]
    for position, name in enumerate(FLAGS):
        out.append(f"    {name} = 1 << {position},")
    out += [
        "};",
        "",
        "// A mapping is what to add to the character or, with HasSequences, where in `sequences` it is << 2 | how many.",
        "struct Record {",
        "    uint16_t flags;",
        "    uint8_t decimal;",
        "    int32_t lower;",
        "    int32_t upper;",
        "    int32_t title;",
        "    int32_t fold;",
        "};",
        "",
        "static constexpr Record records[] = {",
    ]
    for flags, decimal, lower, upper, title, fold in records:
        out.append(f"    {{ {flags:#06x}, {decimal}, {lower}, {upper}, {title}, {fold} }},")
    out += ["};", "", "static constexpr char32_t sequences[] = {"]
    for start in range(0, len(sequences), 16):
        out.append("    " + " ".join(f"{value:#x}," for value in sequences[start:start + 16]))
    out += ["};", "", f"static constexpr unsigned shift = {shift};", ""]
    array(out, "stage1", stage1)
    array(out, "stage2", stage2)
    out += ["} // namespace UnicodeTables", "} // namespace Python", "} // namespace JSC", ""]

    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "parser", "PyUnicodeTables.h")
    with open(path, "w", encoding="utf-8") as file:
        file.write("\n".join(out))
    size = len(stage1) * width(stage1) + len(stage2) * width(stage2) + len(records) * 20 + len(sequences) * 4
    print(f"Unicode {version}: {len(records)} records, shift {shift}, {size} bytes")


if __name__ == "__main__":
    main()
