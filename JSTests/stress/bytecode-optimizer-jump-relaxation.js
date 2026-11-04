//@ runDefault("--useBytecodeOptimizer=1")
// Jump-size relaxation in BytecodeOptimizerAccess::emit(). Each branch jumps over the padding and the next branch.
// With the right padding its offset fits in 8 bits only until the next branch is widened, so each pass widens one more
// branch and the number of branches sets the number of passes. The sweep finds that padding, and covers the pass
// counts on both sides of the one where emit() forces every branch wide.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("expected " + expected + " but got " + actual);
}

function chain(branches, padding) {
    const pad = "odd = !odd; ".repeat(padding);
    let body = "";
    for (let i = branches; i >= 1; --i)
        body += `L${i}: { `;
    body += "if (key === 1) break L1; ";
    for (let i = 2; i <= branches; ++i)
        body += `${pad}if (key === ${i}) break L${i}; } `;
    // The last branch jumps over twice the padding, so it is the first to be widened.
    return new Function("key", "odd", body + pad + pad + "} return odd;");
}

for (let branches = 10; branches <= 20; ++branches) {
    for (let padding = 33; padding <= 45; ++padding) {
        const run = chain(branches, padding);
        const isOdd = pads => pads * padding % 2 === 1;
        shouldBe(run(0, false), isOdd(branches + 1));
        for (let key = 1; key < branches; ++key)
            shouldBe(run(key, false), isOdd(branches));
        shouldBe(run(branches, false), isOdd(branches - 1));
    }
}
