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

const lengths = [100, 300, 16000, 17000, 40000];

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

// The lexer finds the lines while it parses. These are the places where a line ends inside a token, where the
// parser goes back and reads lines again, and where it skips a function that it has read before.
const constructs = [
    t => `/* a${t}b${t}${t}c */`,
    t => `// a${t}`,
    t => `<!-- a${t}`,
    t => "`a" + t + "${" + t + "1}" + t + "b`;",
    t => "String.raw`a" + t + "b`;",
    t => `"a\\${t}b";`,
    t => `'a\\${t}\\${t}b';`,
    t => `(a,${t}b);`,
    t => `(a,${t}b) =>${" "}{${t}return a;${t}};`,
    t => `async (a,${t}b) => a;`,
    t => `(a = function () {${t}${t}return "long enough to be remembered";${t}},${t}b) => a;`,
    t => `({ a,${t}b } = { a,${t}b });`,
    t => `function f() {${t}${t}return function () {${t}return "long enough to be remembered";${t}};${t}}`,
    t => `(class {${t}x =${t}1;${t}static {${t}}${t}m() {${t}}${t}});`,
    t => `a =${t}/x/${t}.test("x");`,
    t => `if (a)${t}b;${t}else${t}a;`,
];

for (const [name, choices] of Object.entries(terminators)) {
    for (const t of choices) {
        let text = "var a, b;" + t;
        const offsets = [];
        // Three times, so that the constructs fall on both sides of the end of a block.
        for (const construct of [...constructs, ...constructs, ...constructs]) {
            text += construct(t) + t;
            offsets.push(text.length + columnInStatement);
            text += `at(new Error(""));` + t;
        }
        // In a string, these two end a line and are characters of the string.
        if (t === "\u2028" || t === "\u2029") {
            text += `"a${t}b";`;
            offsets.push(text.length + columnInStatement);
            text += `at(new Error(""));`;
        }
        shouldBe(positionsOf(text), positionsAt(text, offsets), `constructs, ${name}, ${escape(t)}`);
    }
}

// A parse that fails stops in the middle of the text. The line of the error is asked for right away.
for (const before of [0, 1, 63, 64, 200]) {
    const line = "var someName = 12345678;\n";
    // It only parses, and throws a string that ends with the line. The error of eval() has the line of the call, and
    // a program that does not compile is not for the bytecode cache configuration, where every program has to be cached.
    let message;
    try {
        checkModuleSyntax(line.repeat(before) + "var = ;\n" + line.repeat(100));
    } catch (e) {
        message = e;
    }
    shouldBe(/^SyntaxError: .*:(\d+)$/.exec(message)?.[1], String(before + 1), `a syntax error after ${before} lines`);
}

// A first line whose length, terminator included, is the most that one, two and three bytes hold, and one more.
for (const length of [127, 128, 16383, 16384, 2097151, 2097152]) {
    const text = `"${"x".repeat(length - 4)}";\n` + `at(new Error(""));`;
    shouldBe(text.indexOf("\n") + 1, length, "length of the first line");
    shouldBe(positionsOf(text), `2:${columnInStatement + 1}`, `after a line of ${length}`);
}

for (const lines of [1, 2, 63, 64, 65, 127, 128, 129, 130]) {
    for (const endsWithTerminator of [false, true]) {
        const text = "\n".repeat(lines - 1) + `at(new Error(""));` + (endsWithTerminator ? "\n" : "");
        shouldBe(positionsOf(text), `${lines}:${columnInStatement + 1}`, `${lines} lines`);
    }
}
