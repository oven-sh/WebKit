// The name of a shorthand property starts where it is written, not at the token behind it.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

function errorOf(run) {
    try {
        run();
    } catch (error) {
        return error;
    }
    throw new Error("nothing was thrown");
}

shouldBe(errorOf(() => { let late = { late }; }).message, "Cannot access 'late' before initialization.");
shouldBe(errorOf(() => { let late = { x: 1, late, y: 2 }; }).message, "Cannot access 'late' before initialization.");
shouldBe(errorOf(() => {
    let late = {
        late
    };
}).message, "Cannot access 'late' before initialization.");

let written = errorOf(() => {
    return { x: missingName };
});
let shorthand = errorOf(() => {
    return { missingName };
});
shouldBe(shorthand.message, written.message);
shouldBe(shorthand.line, written.line + 3);
shouldBe(shorthand.column, written.column - "x: ".length);

let lastOfLine = errorOf(() => {
    return {
        missingName
    };
});
shouldBe(lastOfLine.line, shorthand.line + 8);
