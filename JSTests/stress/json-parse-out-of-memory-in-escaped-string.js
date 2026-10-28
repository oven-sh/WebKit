//@ if $buildType == "debug" then runDefault("--maxSingleAllocationSize=1048576") else skip end

// The lexer of JSON.parse gathers a string that has an escape in a buffer, which doubles as it grows.
// When the buffer cannot grow, JSON.parse has to throw the out-of-memory RangeError. It used to crash.
// --maxSingleAllocationSize makes every allocation of more than 1 MiB fail. Every text below is shorter
// than that, because creating a longer one fails too.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: expected ${JSON.stringify(expected)} but got ${JSON.stringify(actual)}`);
}

function shouldThrow(func, expected) {
    let error = null;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (String(error) !== expected)
        throw new Error(`bad error: ${String(error)}, expected ${expected}`);
}

const outOfMemory = "RangeError: Out of memory";
const reviver = (key, value) => value;

const cases = [
    // The buffer has 1024 characters after the first line and a power of two from then on.
    // With 512 Ki characters in it, it asks for 1 Mi and the header of the string.
    { line: "\\n" + "a".repeat(1023), decodedLine: "\n" + "a".repeat(1023), lines: 520 },
    // The same in 16 bits: 512 characters after the first line, and 512 Ki are 1 MiB.
    { line: "\\n" + "あ".repeat(511), decodedLine: "\n" + "あ".repeat(511), lines: 520 },
    // A text of 8-bit characters whose string has 16-bit ones, and so twice the bytes, however the buffer grows.
    { line: "\\u3042" + "a".repeat(600 * 1024), decodedLine: "あ" + "a".repeat(600 * 1024), lines: 1, shortLine: "\\u3042a", decodedShortLine: "あa" },
];

for (const { line, decodedLine, lines, shortLine = line, decodedShortLine = decodedLine } of cases) {
    const long = '"' + line.repeat(lines) + '"';
    const short = '"' + shortLine.repeat(100) + '"';
    const decodedShort = decodedShortLine.repeat(100);

    for (const text of [long, ` ${long} `, `[1,${long}]`, `{"a":${long}}`, `{${long}:1}`, `[{"a":[${short},${long}]}]`]) {
        shouldThrow(() => JSON.parse(text), outOfMemory);
        shouldThrow(() => JSON.parse(text, reviver), outOfMemory);
    }
    shouldThrow(() => JSON.rawJSON(long), outOfMemory);

    // A program that assigns JSON goes to the same lexer before it goes to the parser of programs.
    shouldThrow(() => runString(`assigned = ${long}`), outOfMemory);
    shouldThrow(() => runString(`assigned = {"a":${long}};`), outOfMemory);
    shouldBe(runString(`assigned = {"a":${short}};`).assigned.a, decodedShort);

    // The lexer goes on to the end of the string, so what is wrong inside it is still what is reported.
    const unterminated = long.slice(0, -1);
    shouldThrow(() => JSON.parse(unterminated), "SyntaxError: JSON Parse error: Unterminated string");
    shouldThrow(() => JSON.parse(unterminated + '\\x"'), "SyntaxError: JSON Parse error: Invalid escape character x");

    // A parse that failed leaves nothing behind for the next one.
    shouldBe(JSON.parse(short), decodedShort);
    shouldBe(JSON.parse(`{"a":${short}}`, reviver).a, decodedShort);
    shouldBe(JSON.parse(`{${short}:1}`)[decodedShort], 1);
}
