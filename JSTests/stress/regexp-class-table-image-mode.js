//@ runDefault
//@ runDefault("--useRegExpImageMode=1")
//@ runDefault("--useRegExpJIT=0")

function shouldBe(actual, expected, label) {
    if (JSON.stringify(actual) !== JSON.stringify(expected))
        throw new Error(`${label}: ${JSON.stringify(actual)} instead of ${JSON.stringify(expected)}`);
}

const wideSuffix = "☃";
const forms = [["^C☃$", 1], ["^C+☃$", 3], ["^C*?☃$", 2], ["^C{3}☃$", 3], ["(?<=^C)☃$", 1]];
const pageEdges = [0, 255, 256, 0xd7ff, 0xd800, 0xdbff, 0xdc00, 0xdfff, 0xe000, 0xffff, 0x10000, 0x10ffff];

function checkMembership(source, flags, codePoints, isMember) {
    for (const [form, count] of forms) {
        const regExp = new RegExp(form.replace("C", () => source), flags);
        for (const c of codePoints)
            shouldBe(regExp.test(String.fromCodePoint(c).repeat(count) + wideSuffix), isMember(c), `${regExp} on U+${c.toString(16)}`);
    }
    const regExp = new RegExp(`^${source}$`, flags);
    for (const c of codePoints) {
        if (c < 256)
            shouldBe(regExp.test(String.fromCharCode(c)), isMember(c), `${regExp} on the 8-bit subject U+${c.toString(16)}`);
    }
}

function classSource(ranges, flags, isInverted = false) {
    const escape = flags ? c => `\\u{${c.toString(16)}}` : c => `\\u${c.toString(16).padStart(4, "0")}`;
    const parts = ranges.map(([begin, end]) => begin === end ? escape(begin) : `${escape(begin)}-${escape(end)}`);
    return `[${isInverted ? "^" : ""}${parts.join("")}]`;
}

function isInRanges(ranges, c) {
    for (const [begin, end] of ranges) {
        if (c >= begin && c <= end)
            return true;
    }
    return false;
}

function codePointsNearEdges(ranges, greatest) {
    const result = new Set(pageEdges);
    for (const [begin, end] of ranges) {
        for (const c of [begin - 1, begin, begin + 1, end - 1, end, end + 1, end | 0xff, (end | 0xff) + 1])
            result.add(c);
    }
    return [...result].filter(c => c >= 0 && c <= greatest);
}

function checkClass(ranges, flags, sampledRanges = ranges) {
    const codePoints = codePointsNearEdges(sampledRanges, flags ? 0x10ffff : 0xffff);
    checkMembership(classSource(ranges, flags), flags, codePoints, c => isInRanges(ranges, c));
    checkMembership(classSource(ranges, flags, true), flags, codePoints, c => !isInRanges(ranges, c));
}

const latin1Entries = [[0x30, 0x39], [0x61, 0x7a], [0xe9, 0xe9], [0xf8, 0xff]];
const eightWideEntries = [[0x100, 0x17f], [0x1ff, 0x200], [0x391, 0x3a9], [0x4ff, 0x4ff], [0x5d0, 0x5ea], [0x900, 0x97f], [0x1e00, 0x1eff], [0x1fff, 0x2000]];
const upToSurrogates = [...latin1Entries, ...eightWideEntries, [0x4e00, 0x9fa5], [0xd7f0, 0xd810]];

const classesInBMP = [
    [...latin1Entries, ...eightWideEntries],
    [...upToSurrogates, [0xdbf0, 0xdc0f], [0xdff0, 0xe010], [0xff21, 0xffff]],
    [...latin1Entries, ...eightWideEntries, [0xc000, 0xc0ff], [0xd700, 0xd8ff]],
    [...latin1Entries, ...eightWideEntries.slice(1)],
];
const spreadOverAllPlanes = [];
for (let begin = 0x100; begin + 0x10 <= 0x10ffff; begin += 0x7000)
    spreadOverAllPlanes.push([begin, begin + 0x10]);
const classesBeyondBMP = [
    [...upToSurrogates, [0x2f800, 0x2fa1d]],
    [...upToSurrogates, [0x30000, 0x300ff], [0x40010, 0x40010], [0x50000, 0x5ffff], [0x70000, 0x70001], [0x80080, 0x80180], [0x10fff0, 0x10ffff]],
    [...upToSurrogates, [0xfff0, 0x1000f]],
    spreadOverAllPlanes,
];

for (const ranges of classesInBMP) {
    for (const flags of ["", "u", "v"])
        checkClass(ranges, flags);
}
for (const ranges of classesBeyondBMP) {
    for (const flags of ["u", "v"])
        checkClass(ranges, flags);
}

const distinctBlockPerPage = [];
for (let page = 1; page < 3000; ++page) {
    const begin = 256 * page + page % 128;
    distinctBlockPerPage.push([begin, begin + (page >> 7)]);
}
for (const flags of ["u", "v"])
    checkClass(distinctBlockPerPage, flags, distinctBlockPerPage.filter((range, index) => !(index % 150)));

for (const flags of ["u", "v"]) {
    const surrogates = new RegExp(classSource([...eightWideEntries, [0xd800, 0xdfff]], flags), flags);
    shouldBe(surrogates.test("\u{1f600}"), false, `${surrogates} on a pair`);
    shouldBe(surrogates.exec("\u{1f600}\ud83d").index, 2, `${surrogates} on a lone lead at the end`);
    shouldBe(surrogates.exec("\u{1f600}\ude00").index, 2, `${surrogates} on a lone trail after a pair`);
    shouldBe(surrogates.exec("\ud83dx").index, 0, `${surrogates} on a lone lead before a letter`);
    shouldBe(surrogates.exec("x\ude00").index, 1, `${surrogates} on a lone trail after a letter`);

    const notFace = new RegExp(classSource([...eightWideEntries, [0x1f600, 0x1f600]], flags, true), flags);
    shouldBe(notFace.test("\u{1f600}"), false, `${notFace} on its only non-member`);
    shouldBe(notFace.test("\u{1f600}\u{1f600}"), false, `${notFace} on its only non-member, twice`);
    shouldBe(notFace.exec("\u{1f600}a").index, 2, `${notFace} after its only non-member`);

    const surrogatesAndFace = classSource([...eightWideEntries, [0xd800, 0xdfff], [0x1f600, 0x1f600]], flags);
    shouldBe(new RegExp(`^${surrogatesAndFace}{3}$`, flags).test("\ud83d\u{1f600}\ude00"), true, "a lone lead, a pair, a lone trail");
    shouldBe(new RegExp(`^${surrogatesAndFace}{4}$`, flags).test("\ud83d\u{1f600}\ude00"), false, "a pair is one character");
}

const letters = [0x41, 0x7a, 0xaa, 0xb5, 0xc0, 0xff, 0x100, 0x3b1, 0x5d0, 0x4e00, 0x9fa5, 0xac00, 0xd7a3, 0xfb00, 0xff21, 0x10000, 0x10400, 0x1d400, 0x20000, 0x2a6d6, 0x2f800, 0x2fa1d];
const nonLetters = [0, 0x20, 0x30, 0x40, 0x5b, 0xd7, 0xf7, 0x300, 0x660, 0x2028, 0x2603, 0x3000, 0xd800, 0xdbff, 0xdc00, 0xdfff, 0xe000, 0xff10, 0xffff, 0x1d7ce, 0x1f600, 0xe0001, 0xe0100, 0xf0000, 0x10ffff];
const hanCharacters = [0x2e80, 0x3005, 0x3007, 0x3400, 0x4e00, 0x9fa5, 0xf900, 0x20000, 0x2a6d6, 0x2f800, 0x2fa1d];
const nonHanCharacters = [0x41, 0xe9, 0x3001, 0x3041, 0x30a2, 0xac00, 0xd800, 0xdfff, 0xffff, 0x10000, 0x1f600, 0x10ffff];
for (const flags of ["u", "v"]) {
    checkMembership("\\p{L}", flags, [...letters, ...nonLetters], c => letters.includes(c));
    checkMembership("\\P{L}", flags, [...letters, ...nonLetters], c => !letters.includes(c));
    checkMembership("[^\\p{L}]", flags, [...letters, ...nonLetters], c => !letters.includes(c));
    checkMembership("\\p{Script=Han}", flags, [...hanCharacters, ...nonHanCharacters], c => hanCharacters.includes(c));
}

for (let i = 0; i < 50; ++i) {
    const first = [...eightWideEntries, [0x3000 + 16 * i, 0x3007 + 16 * i]];
    const second = [...eightWideEntries, [0x3008 + 16 * i, 0x300f + 16 * i]];
    const inFirst = String.fromCharCode(0x3000 + 16 * i);
    const inSecond = String.fromCharCode(0x300f + 16 * i);
    for (const flags of ["", "u"]) {
        const regExp = new RegExp(`^${classSource(first, flags)}${classSource(second, flags)}+${classSource(first, flags, true)}$`, flags);
        shouldBe(regExp.test(inFirst + inSecond + inSecond), true, `${regExp} on first, second, second`);
        shouldBe(regExp.test(inFirst + inSecond + inFirst), false, `${regExp} on first, second, first`);
        shouldBe(regExp.test(inSecond + inSecond + inSecond), false, `${regExp} on second, second, second`);
        shouldBe(regExp.test(inFirst + inFirst + inSecond), false, `${regExp} on first, first, second`);
    }
}

for (const [source, flags] of [["[^]", ""], ["[^]", "u"], [".", "s"], [".", "su"]])
    checkMembership(source, flags, pageEdges.filter(c => flags.includes("u") || c <= 0xffff), () => true);

shouldBe(/\s\w/.exec("☃☃ a").index, 2, "\\s\\w after wide characters");
shouldBe(/\s\w/.exec("☃ _").index, 1, "\\s\\w on a line separator");
shouldBe(/\s\w/.test("☃ ☃"), false, "\\s\\w without a word character");
shouldBe(/\S\W/.exec(" 　a☃").index, 2, "\\S\\W after wide white space");
shouldBe(/\d/.exec("☃٥7").index, 2, "\\d after a digit of another script");

shouldBe(/^[acegikmoqsuwy02468\xe9\xf1]+$/.test("ace08\xe9\xf1"), true, "Latin-1 class, 8-bit members");
shouldBe(/^[acegikmoqsuwy02468\xe9\xf1]+$/.test("aceb"), false, "Latin-1 class, 8-bit non-member");
shouldBe(/[acegikmoqsuwy02468\xe9\xf1]/.exec("☃bš\xf1").index, 3, "Latin-1 class, 16-bit subject");

shouldBe(/(\xe9)\1/i.exec("x\xe9\xc9"), ["\xe9\xc9", "\xe9"], "backreference, 8-bit subject");
shouldBe(/(\xe9)\1/i.test("\xe9e"), false, "backreference, 8-bit subject, other letter");
shouldBe(/(\xe9)\1/i.exec("☃\xc9\xe9").index, 1, "backreference, Latin-1 letters in a 16-bit subject");
shouldBe(/(я)\1/i.test("яЯ"), true, "backreference, BMP letters");
shouldBe(/(я)\1/i.test("яЮ"), false, "backreference, BMP letters, other letter");
shouldBe(/(\u{10400})\1/iu.test("\u{10400}\u{10428}"), true, "backreference, upper then lower beyond the BMP");
shouldBe(/(\u{10400})\1/iu.test("\u{10428}\u{10400}"), true, "backreference, lower then upper beyond the BMP");
shouldBe(/(\u{10400})\1/iu.test("\u{10400}\u{10429}"), false, "backreference beyond the BMP, other letter");

for (const filler of ["x", "☃"]) {
    shouldBe(/needle01|haystack/.exec(filler.repeat(200) + "haystack").index, 200, "search for two words");
    shouldBe(/abcdefghijklmnop/i.exec(filler + "ABCDEFGHijklmnop").index, 1, "sixteen letters ignoring case");
    shouldBe(/abcdefghijklmnop/i.test(filler + "ABCDEFGHijklmnoq"), false, "sixteen letters ignoring case, last differs");
}

for (const count of [11, 12]) {
    const text = "abcdefghijkl".slice(0, count);
    const repeated = new RegExp(`^(?:${[...text].map(letter => `(${letter})?`).join("")},)*$`);
    shouldBe(repeated.exec(`${text},a,`), [`${text},a,`, "a", ...Array(count - 1).fill(null)], `${repeated}: an iteration clears the captures of the one before`);
    const optional = new RegExp(`^(?:${[...text].map(letter => `(${letter})`).join("")})?${text.slice(0, -1)}$`);
    shouldBe(optional.exec(text.slice(0, -1)), [text.slice(0, -1), ...Array(count).fill(null)], `${optional}: a group that failed leaves no capture`);
}

shouldBe(/^.\u{1f600}.$/u.test("\u{1f4a9}\u{1f600}\ud800"), true, "two pairs and a lone lead");
shouldBe(/^[^a]$/u.test("\u{1f600}"), true, "a pair is one character");
shouldBe(/^[^a]$/u.test("\ud83d"), true, "a lone lead is one character");
shouldBe(/^.$/u.test("\ud83d\ud83d"), false, "two lone leads are two characters");
shouldBe(/^..$/u.test("\u{1f600}"), false, "a pair is not two characters");
