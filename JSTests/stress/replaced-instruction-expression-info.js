//@ runDefault
//@ runDefault("--useBytecodeOptimizer=1")
// These peepholes replace the last instruction after expression info has been recorded at its end:
//   `{ let copy = value; }`      mov copy, <empty>   ->  mov copy, value
//   `copy = far; copy = value;`  mov copy, far       ->  mov copy, value
//   `typeof value > "u"`         typeof tmp, value   ->  typeof_is_undefined result, value
// The replaced instruction is wide when <empty> is past the constants, far past the arguments, or tmp past the locals,
// that narrow operands reach.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

function names(prefix, count) {
    return Array.from({ length: count }, (_, i) => prefix + i);
}

function constants(count) {
    return { parameters: [], source: `sink(); var last; ${names("last = ", count).join("; ")};` };
}

function locals(count) {
    return { parameters: [], source: `sink(); var ${names("local", count).join(", ")};` };
}

function parameters(count) {
    return { parameters: [...names("parameter", count), "far"], source: "sink();" };
}

function positionOfThrow({ parameters, source }, tail) {
    let test = new Function("value", "result", "sink", ...parameters, `${source}\n${tail}`);
    try {
        test(undefined, undefined, () => { });
    } catch (error) {
        return `${error.line}:${error.column}`;
    }
    throw new Error("did not throw");
}

let tests = [
    [constants, "{ let copy = value; copy.x; }"],
    [constants, "for (let copy = value; copy.x; ) { }"],
    [parameters, "var copy = far; copy = value; copy.x;"],
    [locals, `result = typeof value > "u"; value.x;`],
];

for (let [prologue, tail] of tests) {
    let expected = positionOfThrow(prologue(1), tail);
    for (let count of [5, 10, 100, 110, 115, 120, 125, 130, 200, 33000])
        shouldBe(positionOfThrow(prologue(count), tail), expected);
}
