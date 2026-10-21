//@ runDefault
//@ runDefault("--useBytecodeOptimizer=1")
// These peepholes replace the last instruction after expression info has been recorded at its end:
//   `{ let copy = value; }`    mov copy, <empty>   ->  mov copy, value
//   `typeof value > "u"`       typeof tmp, value   ->  typeof_is_undefined result, value
// The replaced instruction is wide when <empty> is past the constants, or tmp past the locals, that narrow operands reach.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

function constants(count) {
    let source = "sink(); var last;";
    for (let i = 0; i < count; ++i)
        source += ` last = ${i};`;
    return source;
}

function locals(count) {
    let names = [];
    for (let i = 0; i < count; ++i)
        names.push(`local${i}`);
    return `sink(); var ${names.join(", ")};`;
}

function positionOfThrow(prologue, tail) {
    let test = new Function("value", "result", "sink", `${prologue}\n${tail}`);
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
    [locals, `result = typeof value > "u"; value.x;`],
];

for (let [prologue, tail] of tests) {
    let expected = positionOfThrow(prologue(1), tail);
    for (let count of [100, 110, 115, 120, 125, 130, 200, 70000])
        shouldBe(positionOfThrow(prologue(count), tail), expected);
}
