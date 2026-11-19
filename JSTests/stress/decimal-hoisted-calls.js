//@ requireOptions("--useDecimal=1", "--maximumInliningDepth=1")
// A call without effects may be made in front of a loop, and so in front of the checks that guard it.

function construct(isEarly, argument) {
    function inLoop(constructs) {
        let result;
        for (let i = 0; i < 3; ++i) {
            if (constructs)
                result = new Decimal(late);
        }
        return result;
    }
    if (isEarly)
        return inLoop(false);
    let late = argument;
    return inLoop(true);
}
noInline(construct);

const functions = ["round(count).divide(y)", "scale10(count).divide(y)", "round(count).remainder(y)", "scale10(count).mantissa().multiply(y)", "round(count).divide(y).round(1)"].map(source => {
    const f = new Function("y", "count", "computes", `let result = y.isNaN(); for (let i = 0; i < 3; ++i) { if (computes) result = new Decimal("1.25").${source}; } return result;`);
    noInline(f);
    return f;
});

for (let i = 0; i < testLoopCount * 20; ++i) {
    const isIdle = i & 1;
    if (construct(isIdle, i & 2 ? "2.5" : 2.5)?.toString() !== (isIdle ? undefined : "2.5"))
        throw new Error("construct");
    for (const f of functions) {
        if (f(new Decimal(1), isIdle ? "no" : i & 2 ? 1 : 2 ** 40, !isIdle) instanceof Decimal === !!isIdle)
            throw new Error(String(f));
    }
}
