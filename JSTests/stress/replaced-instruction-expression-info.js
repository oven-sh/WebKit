//@ runDefault
//@ runDefault("--useBytecodeOptimizer=1")
// `{ let copy = value; ... }` emits `mov copy, <empty>`, records expression info, then emits `mov copy, value`, which
// replaces the first mov. Narrow operands reach 112 constants, so with more of them the first mov is wide.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

function positionOfThrow(constantCount, tail) {
    let numbers = [];
    for (let i = 1; i <= constantCount; ++i)
        numbers.push(i);
    let test = new Function("value", "sink", `sink(${numbers.join(", ")});\n${tail}`);
    try {
        test(undefined, () => { });
    } catch (error) {
        return `${error.line}:${error.column}`;
    }
    throw new Error("did not throw");
}

for (let tail of ["{ let copy = value; copy.x; }", "for (let copy = value; copy.x; ) { }"]) {
    let expected = positionOfThrow(1, tail);
    for (let constantCount of [110, 111, 112, 113, 200, 70000])
        shouldBe(positionOfThrow(constantCount, tail), expected);
}
