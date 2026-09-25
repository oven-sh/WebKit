function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
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
    return `${match[1]}:${match[2]}`;
}

const expected = ["7:38", "9:25", "11:18"];
for (let i = 0; i < 100; i++) {
    for (let which = 0; which < 3; which++)
        shouldBe(positionOfFirstFrame(makeStack(which)), expected[which]);
}
