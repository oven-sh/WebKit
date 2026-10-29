//@ runDefault
//@ runDefault("--useRegExpImageMode=1")
//@ runDefault("--useRegExpJIT=0")

load("./resources/regexp-reference-matcher.js", "caller relative");

const first = "\u{10000}";
const second = "\u{10001}";
const otherLead = "\u{10400}";

const isLineTerminator = unit => unit === "\n" || unit === "\r" || unit === " " || unit === " ";
const firstPresent = (subject, index, candidates) => candidates.find(candidate => subject.startsWith(candidate, index)) ?? null;

function repeatedFirst(subject, index) {
    let end = index;
    while (subject.startsWith(first, end))
        end += first.length;
    return subject.slice(index, end);
}

// Each function returns the text the pattern matches at a code point boundary, or null.
const everyMatchBeginsWithOneLead = [
    ["\\u{10000}", "u", (subject, index) => firstPresent(subject, index, [first])],
    ["\\u{10000}?\\u{10001}", "u", (subject, index) => firstPresent(subject, index, [first + second, second])],
    ["^\\u{10000}", "mu", (subject, index) => !index || isLineTerminator(subject[index - 1]) ? firstPresent(subject, index, [first]) : null],
    ["(?<=a)\\u{10000}", "u", (subject, index) => subject[index - 1] === "a" ? firstPresent(subject, index, [first]) : null],
    ["(?:\\u{10000}|\\u{10001})x", "u", (subject, index) => firstPresent(subject, index, [first + "x", second + "x"])],
    ["\\u{10000}x|\\u{10001}", "u", (subject, index) => firstPresent(subject, index, [first + "x", second])],
];
const matchesBeginDifferently = [
    ["\\u{10000}|\\u{10400}", "u", (subject, index) => firstPresent(subject, index, [first, otherLead])],
    ["\\u{10000}|a", "u", (subject, index) => firstPresent(subject, index, [first, "a"])],
    ["\\u{10000}*", "u", repeatedFirst],
    ["\\u{10000}?x", "u", (subject, index) => firstPresent(subject, index, [first + "x", "x"])],
];

function expectedResult(subject, matchAt) {
    for (let index = 0; index <= subject.length; index += subject.codePointAt(index) > 0xffff ? 2 : 1) {
        const text = matchAt(subject, index);
        if (text !== null)
            return { index, captures: [text] };
    }
    return null;
}

const subjects = stringsOver([first, second, otherLead, "x", "a", "\n", "\ud800", "\udc00"], 4);
for (const filler of ["x", "Ж", second, otherLead, "\ud800", "\udc00"]) {
    for (const length of [1, 7, 8, 15, 16, 17, 33, 64]) {
        const skipped = filler.repeat(length);
        subjects.push(skipped, skipped + first, skipped + "a" + first + "x", skipped + "\n" + first + second + "x", skipped + otherLead + "a");
    }
}

for (const [source, flags, matchAt] of [...everyMatchBeginsWithOneLead, ...matchesBeginDifferently]) {
    const regexp = new RegExp(source, flags);
    for (const subject of subjects)
        ReferenceRegExp.checkYarr(regexp, subject, 0, expectedResult(subject, matchAt));
}
