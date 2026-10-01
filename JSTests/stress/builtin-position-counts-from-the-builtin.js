//@ runDefault("--useDollarVM=1")

// The builtins share one text, and none of them knows of the others: a line and column in a builtin counts from where
// the builtin starts. Finding it does not read the rest of the text.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

function thrownBy(f) {
    try {
        f();
    } catch (e) {
        return e;
    }
    throw new Error("did not throw");
}

// A builtin has no expression info, so an error in it is where its parameters start: after "(function ". A build with
// assertions keeps the expression info, and the error is where it is thrown in the builtin. In the text of all the
// builtins the line would be in the thousands.
for (const f of [() => [].reduce(() => { }), () => [].reduceRight(() => { }), () => Array.prototype.map.call(null)]) {
    const error = thrownBy(f);
    if ($vm.assertEnabled())
        shouldBe(error.line < 100, true, String(f));
    else
        shouldBe(`${error.line}:${error.column}`, "1:11", String(f));
}

shouldBe($vm.lineStartTableIsBuilt(Array.prototype.reduce), false, "no table");
