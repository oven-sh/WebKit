//@ runDefault
//@ runDefault("--useRegExpImageMode=1")
//@ runDefault("--useRegExpJIT=0")
//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")

load("./resources/regexp-reference-matcher.js", "caller relative");

function shouldBe(actual, expected, label) {
    if (actual !== expected)
        throw new Error(label + ": " + actual + " instead of " + expected);
}

function nearStackLimit(run) {
    try {
        return nearStackLimit(run);
    } catch (error) {
        if (!(error instanceof RangeError))
            throw error;
        return run();
    }
}

const pairs = [
    ["a+b", "a+ab", "ab"],
    ["a*b", "a*ab", "ab"],
    ["a?b", "a?ab", "ab"],
    ["a+b", "a+?b", "ab"],
    ["a+b", "c*a+b", "abc"],
    ["a+$", "a+a$", "ab"],
    ["^a+b", "^a+ab", "ab"],
    ["\\ba+-", "\\ba+a-", "a-"],
    ["[ab]+c", "[ab]+bc", "abc"],
    ["[^c]+c", "[^c]+.c", "abc"],
    ["ca+b", "ca+ab", "abc"],
    ["a{2,}b", "a{2,}ab", "ab"],
    ["a{1,3}b", "a{1,3}ab", "ab"],
    ["a+b+c", "a+b+bc", "abc"],
    ["(?:ab|cb)c", "(?:ab|a)bc", "abc"],
    ["(?:ab|c)a", "b*(?:ab|c)a", "abc"],
    ["(?:ab)?c", "(?:ab)?ac", "abc"],
    ["(?:ab)*c", "(?:ab)*ac", "abc"],
    ["(?:ab)+c", "(?:ab)+ac", "abc"],
    ["c(?:ab)*", "c(?:ab)*a", "abc"],
    ["(ab|cb)c", "(ab|a)bc", "abc"],
    ["(a+)b", "(a+)ab", "ab"],
    ["(a)?b", "(a)?ab", "ab"],
    ["(a+)(b+)c", "(a+)(b+)bc", "abc"],
    ["a+b|b+c", "a+ab|b+bc", "abc"],
    ["a+b", "(?=a)a+b", "ab"],
    ["a+b", "(?!b)a+b", "ab"],
    ["(a)b", "(a)\\1b", "ab"],
    ["[ac]b", "[ac]b+b", "abc"],
    ["[ac]b+c", "[ac]b+bc", "abc"],
];

for (const [framelessSource, framedSource, alphabet] of pairs) {
    const first = alphabet[0];
    const second = alphabet[1];
    const longRuns = [17, 70].flatMap(count => [first.repeat(count), first.repeat(count) + second, second.repeat(count) + first + second]);
    const stackLimitSubject = first.repeat(20) + second + alphabet.at(-1);
    const subjects = stringsOver(alphabet, alphabet.length === 2 ? 6 : 5).concat(longRuns, stackLimitSubject);
    const shortSubjects = stringsOver(alphabet, 3);

    for (const source of [framelessSource, framedSource]) {
        for (const flags of ["", "i", "u"]) {
            checkRegExp(source, flags, subjects);
            checkRegExp(source, flags, shortSubjects, { allStartIndexes: true });
        }

        const regexp = new RegExp(source);
        const expectedMatch = JSON.stringify(regexp.exec(stackLimitSubject));
        shouldBe(JSON.stringify(nearStackLimit(() => regexp.exec(stackLimitSubject))), expectedMatch, "exec of /" + source + "/ near the stack limit");
        const expectedTest = regexp.test(stackLimitSubject);
        shouldBe(nearStackLimit(() => regexp.test(stackLimitSubject)), expectedTest, "test of /" + source + "/ near the stack limit");
    }
}
