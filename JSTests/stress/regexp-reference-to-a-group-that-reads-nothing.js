//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")
//@ runDefault("--useRegExpJIT=0", "--useRegExpImageMode=1")
//@ runDefault

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": expected " + expected + ", got " + actual);
}

function check(pattern, subject, expected) {
    const match = pattern.exec(subject);
    shouldBe(match && match.index + ":" + Array.from(match).join(), expected, pattern + " on " + JSON.stringify(subject));
}

// A group that holds only a lookahead takes nothing, so the reference behind the run reads nothing.
check(/((?=a))aZ*\1Z/, "aZZ", "0:aZZ,");
check(/((?=a))aZ*?\1Z/, "aZZ", "0:aZ,");
check(/((?=a)|b)aZ*\1Z/, "aZZ", "0:aZZ,");
check(/((?:(?=a)))aZ*\1Z/, "aZZ", "0:aZZ,");
check(/((?=a)(?=a))aZ*\1Z/, "aZZ", "0:aZZ,");
check(/((?=a)Y*)aZ*\1Z/, "aZZ", "0:aZZ,");
check(/((?=a)a)aZ*\1Z/, "aaZZaZ", "0:aaZZaZ,a");

// A group still open behind a run that began at the end of the input has no first unit.
for (let length = 0; length < 25; ++length) {
    const subject = Array.from({ length }, (_, i) => String.fromCharCode(97 + i)).join("");
    for (const text of [subject, subject + "\u0100"]) {
        const atEnd = text.length + ":,";
        check(/(.*)\1$/, text, atEnd);
        check(/(.*?)\1$/, text, atEnd);
        check(/^(.*)\1$/, text, text.length ? null : "0:,");
        check(/(.+?)\1/, text, null);
    }
}
