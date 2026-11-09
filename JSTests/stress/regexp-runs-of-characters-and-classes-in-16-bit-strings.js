//@ runDefault
//@ runDefault("--useRegExpImageMode=1")
//@ runDefault("--useRegExpImageMode=1", "--verifyRegExpJITReads=1")
//@ runDefault("--useRegExpJIT=0")
//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")

// Mixes of literal characters and single-character classes in a run whose fifth position is a class, in 8-bit and 16-bit strings.
// Every seventh mix is tried, and at length 12 without flags also every mix that used to be matched wrongly in 16-bit strings.
// The native test regexp-image/every-shape-of-run-matches-wide-subjects tries all of them.
// In image mode a process has room for 64 MB of compiled patterns, at least 8 KB each, and falls back to the interpreter after that.
// So keep this to fewer than 8,000 patterns, and keep \w, \s and \b out of the loop.

function shouldBe(pattern, subject, expected) {
    if (pattern.test(subject) !== expected)
        throw new Error("/" + pattern.source + "/" + pattern.flags + " in " + JSON.stringify(subject) + ": not " + expected);
}

const letters = "abcdefghijklm";

function patternOfShape(shape, length) {
    let source = "a";
    for (let place = 1; place < length; ++place)
        source += (shape >> (place - 1)) & 1 ? "[a-z]" : letters[place];
    return source;
}

// A class at the fifth and eighth positions, a literal at the sixth and the last.
function usedToBeMatchedWrongly(shape) {
    return (shape & 0b10001011000) === 0b00001001000;
}

for (const [length, flags] of [[7, ""], [12, ""], [12, "i"], [12, "u"], [12, "v"], [13, ""]]) {
    const text = letters.slice(0, length);
    for (let shape = 0; shape < 1 << (length - 1); ++shape) {
        if (length > 11 && !(shape & 1 << 3))
            continue;
        if (shape % 7 && !(length === 12 && !flags && usedToBeMatchedWrongly(shape)))
            continue;
        const pattern = new RegExp(patternOfShape(shape, length), flags);
        shouldBe(pattern, text, true);
        shouldBe(pattern, text + "\u2013", true);
        shouldBe(pattern, "\u2013" + text, true);
        const place = shape % length;
        shouldBe(pattern, text.slice(0, place) + "~" + text.slice(place + 1) + "\u2013", false);
    }
}

for (const [pattern, text] of [
    [/T\d\d:\d\d:\d\d\+\d\d:\d\d/, "T03:00:59+01:00"],
    [/T\d\d:\d\d:\d\d\.00Z/, "T03:00:59.00Z"],
    [/utc[+-]\d\d:\d\d end/i, "UTC+01:00 END"],
    [/.T\d\d:\d\d:\d\d\+\d\d:\d\d/, "7T03:00:59+01:00"],
    [/(\d{4})-\d\d-\d\d \d\d:\d\d:\d\d/, "2001-02-03 03:00:59"],
    [/\d+-\d\d-\d\d \d\d:\d\d:\d\d/, "2001-02-03 03:00:59"],
    [/\w+ \d\d \d\d:\d\d:\d\d /, "Feb 03 03:00:59 host"],
    [/\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d/, "2001-02-03T03:00:59"],
]) {
    shouldBe(pattern, text, true);
    shouldBe(pattern, text + "\u2013", true);
}

// The second character after the class.
shouldBe(/abcd[\0x]ef[xy]ghij/, "abcd\0eZxghij\u2013", false);
shouldBe(/abcd[\0x]ef[xy]ghij/, "abcd\0efxghij\u2013", true);
