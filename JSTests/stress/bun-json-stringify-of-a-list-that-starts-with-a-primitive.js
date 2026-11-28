// A list that starts with a string, a number or a literal has a loop of its own in the fork's JSON.stringify
// (FastStringifier::appendContiguousArrayThatStartsWithPrimitive). What it writes is what the general writer writes:
// for elements of every kind behind the first, with and without an indent, in 8-bit and in 16-bit results, and in
// results that fit the static buffer and results that do not.
function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": got " + actual + ", expected " + expected);
}
// The reference: a writer in JavaScript, for the values of this test.
function reference(value, indent, depth = 0) {
    if (value === null || value === undefined || typeof value === "function" || typeof value === "symbol")
        return "null";
    if (typeof value === "string")
        return quote(value);
    if (typeof value === "number")
        return Number.isFinite(value) ? String(value) : "null";
    if (typeof value === "boolean")
        return String(value);
    const inner = indent ? "\n" + indent.repeat(depth + 1) : "";
    const outer = indent ? "\n" + indent.repeat(depth) : "";
    if (Array.isArray(value)) {
        if (!value.length)
            return "[]";
        const parts = [];
        for (let i = 0; i < value.length; i++)
            parts.push(reference(value[i], indent, depth + 1));
        return "[" + inner + parts.join("," + inner) + outer + "]";
    }
    const parts = [];
    for (const key of Object.keys(value)) {
        const v = value[key];
        if (v === undefined || typeof v === "function" || typeof v === "symbol")
            continue;
        parts.push(quote(key) + (indent ? ": " : ":") + reference(v, indent, depth + 1));
    }
    if (!parts.length)
        return "{}";
    return "{" + inner + parts.join("," + inner) + outer + "}";
}
function quote(string) {
    let out = '"';
    for (let i = 0; i < string.length; i++) {
        const unit = string[i];
        const code = string.charCodeAt(i);
        const isLead = code >= 0xd800 && code <= 0xdbff;
        const isTrail = code >= 0xdc00 && code <= 0xdfff;
        const nextIsTrail = i + 1 < string.length && string.charCodeAt(i + 1) >= 0xdc00 && string.charCodeAt(i + 1) <= 0xdfff;
        if (unit === '"') out += '\\"';
        else if (unit === "\\") out += "\\\\";
        else if (unit === "\n") out += "\\n";
        else if (unit === "\t") out += "\\t";
        else if (unit === "\r") out += "\\r";
        else if (unit === "\b") out += "\\b";
        else if (unit === "\f") out += "\\f";
        else if (isLead && nextIsTrail) out += unit + string[++i];
        else if (code < 0x20 || isLead || isTrail) out += "\\u" + code.toString(16).padStart(4, "0");
        else out += unit;
    }
    return out + '"';
}
function check(value, what) {
    for (const indent of [undefined, 2, "\t"]) {
        const gap = indent === 2 ? "  " : indent;
        // Twice and more: the second call of a shape takes the prepared path where there is one.
        for (let round = 0; round < 3; round++)
            shouldBe(JSON.stringify(value, null, indent), reference(value, gap), what + " (indent " + JSON.stringify(indent) + ", round " + round + ")");
    }
}
const object = i => ({ id: i, name: "n" + i });
const long = "x".repeat(5000);
const lists = {
    "one string": ["a"],
    "strings": ["a", "b", "c"],
    "numbers in a list that held a string": (() => { const a = ["x", 1, 2, 3]; a.shift(); return a; })(),
    "a string and numbers": ["a", 1, 2.5, -0, 1e21, NaN, Infinity],
    "literals": [true, false, null, undefined, true],
    "undefined first": [undefined, "a", undefined],
    "null first, then objects": [null, object(1), object(2), object(3)],
    "a string, then objects of one shape": ["head", object(1), object(2), object(3), object(4)],
    "a string, then lists": ["head", ["a", "b"], [1, 2], [], [object(1)], [[["deep"]]]],
    "a string, then an empty object and an empty list": ["head", {}, []],
    "strings with escapes": ["plain", 'quo"te', "back\\slash", "new\nline", "tab\t", "\u0001", "\u001f"],
    "16-bit strings": ["latin", "\u00e9t\u00e9", "\u4e2d\u6587", "\ud83d\ude00", "\ud800"],
    "a long string first": [long, "short", long],
    "many strings": Array.from({ length: 3000 }, (_, i) => "text " + i),
    "many numbers behind a string": ["s", ...Array.from({ length: 3000 }, (_, i) => i * 1.5)],
    "many 16-bit strings": Array.from({ length: 3000 }, (_, i) => "\u4e2d" + i),
    "a function and a symbol": ["a", function () { }, Symbol("s"), "b"],
    "nested in an object": { tags: ["a", "b"], more: { tags: ["c"], none: [], numbers: ["n", 1, 2] } },
    "objects that hold lists of strings": Array.from({ length: 20 }, (_, i) => ({ id: i, tags: ["t" + i, "u" + i, i] })),
    "a rope first": ["left" + String(Math.random()).slice(0, 0) + "right".repeat(3), "b"],
};
for (const [what, value] of Object.entries(lists))
    check(value, what);

// A hole is left to the general writer.
const withHole = ["a", , "c"];
shouldBe(JSON.stringify(withHole), '["a",null,"c"]', "a hole");
shouldBe(JSON.stringify(withHole, null, 1), '[\n "a",\n null,\n "c"\n]', "a hole, indented");
shouldBe(JSON.stringify({ list: withHole }), '{"list":["a",null,"c"]}', "a hole in a nested list");

// toJSON and a getter behind a string are found.
const withToJSON = ["a", { toJSON() { return "from toJSON"; } }];
shouldBe(JSON.stringify(withToJSON), '["a","from toJSON"]', "toJSON behind a string");
let reads = 0;
const withGetter = ["a", { get value() { reads++; return 7; } }];
for (let round = 0; round < 3; round++)
    shouldBe(JSON.stringify(withGetter), '["a",{"value":7}]', "a getter behind a string");
shouldBe(reads, 3, "reads of the getter");

// A BigInt behind a string throws.
let threw = false;
try { JSON.stringify(["a", 1n]); } catch (error) { threw = error instanceof TypeError; }
shouldBe(threw, true, "a BigInt behind a string");

// A list that is its own element throws.
const ring = ["a"]; ring.push(ring);
threw = false;
try { JSON.stringify(ring); } catch (error) { threw = error instanceof TypeError; }
shouldBe(threw, true, "a ring behind a string");

// Lists in lists, 2000 deep.
let deep = ["leaf"];
for (let i = 0; i < 2000; i++) deep = ["s", deep];
shouldBe(JSON.stringify(deep), reference(deep), "2000 levels");
