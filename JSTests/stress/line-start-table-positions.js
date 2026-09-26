// The line and column of an error come from its source's line start table. This compares them with
// positions counted here, for every kind of line terminator, for line lengths on both sides of what
// one, two and three bytes of the table's length encoding hold, and for line counts around its block size.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

let seed = 12345;
function random(n) {
    seed = (seed * 1103515245 + 12345) & 0x7fffffff;
    return (seed >>> 12) % n;
}

globalThis.seen = [];
globalThis.at = function (error) {
    seen.push(`${error.line}:${error.column}`);
};

function positionsOf(text) {
    seen.length = 0;
    (0, eval)(text);
    return seen.join(" ");
}

// One-based, for ascending offsets, by reading the text one character at a time.
function positionsAt(text, offsets) {
    const positions = [];
    let line = 1;
    let lineStart = 0;
    let i = 0;
    for (const offset of offsets) {
        for (; i < offset; i++) {
            const c = text[i];
            if (c === "\r" && text[i + 1] === "\n")
                i++;
            else if (c !== "\n" && c !== "\r" && c !== "\u2028" && c !== "\u2029")
                continue;
            line++;
            lineStart = i + 1;
        }
        positions.push(`${line}:${offset - lineStart + 1}`);
    }
    return positions.join(" ");
}

// Where in `at(new Error("..."));` the column of the error is.
const columnInStatement = Number(positionsOf(`at(new Error(""));`).split(":")[1]) - 1;

const terminators = {
    "LF": ["\n"],
    "CR LF": ["\r\n"],
    "CR": ["\r"],
    "LF, CR LF and CR": ["\n", "\r\n", "\r"],
    "all five, 16-bit": ["\n", "\r\n", "\r", "\u2028", "\u2029"],
};

const lengths = [126, 127, 128, 129, 300, 16382, 16383, 16384, 16385, 40000];

for (const [name, choices] of Object.entries(terminators)) {
    const terminator = () => choices[random(choices.length)];
    let text = "";
    const offsets = [];
    for (let i = 0; i < 150; i++) {
        for (let blank = random(4); blank > 0; blank--)
            text += terminator();
        text += " ".repeat(random(12));
        offsets.push(text.length + columnInStatement);
        text += `at(new Error("${"x".repeat(i < lengths.length ? lengths[i] : random(60))}"));` + terminator();
    }
    const expected = positionsAt(text, offsets);
    shouldBe(positionsOf(text), expected, name);
    // Again: the answers are kept.
    shouldBe(positionsOf(text), expected, `${name}, second time`);
}

for (const lines of [1, 2, 63, 64, 65, 127, 128, 129, 130]) {
    for (const endsWithTerminator of [false, true]) {
        const text = "\n".repeat(lines - 1) + `at(new Error(""));` + (endsWithTerminator ? "\n" : "");
        shouldBe(positionsOf(text), `${lines}:${columnInStatement + 1}`, `${lines} lines`);
    }
}
