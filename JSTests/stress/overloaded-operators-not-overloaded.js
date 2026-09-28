load("./resources/overloaded-operators.js", "caller relative");

run([
    ["a === b", (a, b) => a === b, false],
    ["a !== b", (a, b) => a !== b, true],
    ["a === a", a => a === a, true],
    ["a == null", a => a == null, false],
    ["a != undefined", a => a != undefined, true],
    ["a == nothing", (a, b, nothing) => a == nothing, false],
    ["nothing != a", (a, b, nothing) => nothing != a, true],
    ["+a", a => +a, 6],
    ["+a * 2", a => +a * 2, 12],
    ["2 - +a", a => 2 - +a, -4],
    ["+a / +b", (a, b) => +a / +b, 2],
    ["a >>> 1", a => a >>> 1, 3],
    ["(a >>> 1) < b", (a, b) => (a >>> 1) < 4, true],
    ["`${a}`", a => `${a}`, "W6"],
]);

if (asked.length)
    throw new Error("Asked for " + asked);

// Taking the + off an operand is for a number only.
{
    const shouldThrow = f => {
        for (let i = 0; i < testLoopCount; i++) {
            let threw = false;
            try {
                f(1n, 2n);
            } catch (e) {
                threw = e instanceof TypeError;
            }
            if (!threw)
                throw new Error(`${f} did not throw`);
        }
    };
    shouldThrow((a, b) => +a * b);
    shouldThrow((a, b) => a * +b);
    shouldThrow((a, b) => +a / b);
    shouldThrow((a, b) => +a % b);
    shouldThrow((a, b) => +a - b);
    shouldThrow((a, b) => a * 1);
    shouldThrow((a, b) => 1 * a);
}

// 0n equals 0 and is not the same.
{
    const strict = (a, b) => (a & b) === 0 ? "then" : "else";
    const notStrict = (a, b) => (a & b) !== 0 ? "then" : "else";
    const loose = (a, b) => (a & b) == 0 ? "then" : "else";
    for (let i = 0; i < testLoopCount; i++) {
        if (strict(1n, 2n) !== "else" || notStrict(1n, 2n) !== "then" || loose(1n, 2n) !== "then")
            throw new Error("0n compared with 0");
        if (strict(1, 2) !== "then" || notStrict(1, 2) !== "else" || loose(1, 2) !== "then")
            throw new Error("0 compared with 0");
    }
}
