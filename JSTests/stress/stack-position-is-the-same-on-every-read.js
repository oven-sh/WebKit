function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${JSON.stringify(expected)} but got ${JSON.stringify(actual)}`);
}

function makeStack(which) {
    if (which === 0) return new Error("a").stack;
    if (which === 1)
        return new Error("b").stack;
    const error = (
        new Error("c")
    );
    return error.stack;
}

function positionOfFirstFrame(stack) {
    const match = /^makeStack@.*:(\d+):(\d+)$/m.exec(stack);
    return [Number(match[1]), Number(match[2])];
}

const expected = [[7, 38], [9, 25], [11, 18]];
for (let which = 0; which < 3; which++) {
    const first = makeStack(which);
    shouldBe(JSON.stringify(positionOfFirstFrame(first)), JSON.stringify(expected[which]));
    for (let i = 0; i < 100; i++)
        shouldBe(makeStack(which), first);
}
