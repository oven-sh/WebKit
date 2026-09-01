//@ requireOptions("--useJSThreads=1", "--validateButterflyTagDiscipline=1")
// The butterfly tag-discipline lint must accept inline-offset GetByOffset /
// PutByOffset / GetGetterSetterByOffset: for an inline offset the storage
// child is the base cell itself, not a butterfly word, so there is no tag to
// mask. Before the fix the lint reported an I14 violation for every such
// access whose base came from GetLocal/NewObject/... and the RELEASE_ASSERT
// aborted the DFG compile.

function inlineGet(o) { return o.a; }
noInline(inlineGet);

function inlinePut(o, v) { o.a = v; }
noInline(inlinePut);

function inlineGetter(o) { return o.g; }
noInline(inlineGetter);

function makeWithGetter(i) {
    return { a: i, get g() { return this.a + 1; } };
}

for (let i = 0; i < 20000; ++i) {
    const o = { a: i, b: 2 };
    inlinePut(o, i + 1);
    if (inlineGet(o) !== i + 1)
        throw new Error("inline get/put mismatch at " + i);
    const w = makeWithGetter(i);
    if (inlineGetter(w) !== i + 1)
        throw new Error("inline getter mismatch at " + i);
}
