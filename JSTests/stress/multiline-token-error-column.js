function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error('bad value: ' + actual);
}

{
    let { line, column } = (function() {/*
      something about this comment means the line number gets reported incorrectly in the stack
      */const e = new Error("new error"); return e;
    })();
    shouldBe(line, 9);
    shouldBe(column, 19); // Bun: upstream has 28, where `Error` ends. `new X(args)` is reported at its `new`.
}
{
    let { line, column } = (function() {
        let s = `
multi
line
string
`; const e = new Error("new error"); return e;
    })();
    shouldBe(line, 20);
    shouldBe(column, 14); // Bun: upstream has 23, where `Error` ends.
}
