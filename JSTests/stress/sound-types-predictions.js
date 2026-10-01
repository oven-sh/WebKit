//@ requireOptions("--useSoundTypes=1")

// The checks bound the predictions of arguments, so callers that pass values the checks reject do not cost the code after the
// checks its types. This only tests that doing so is not observable; JSTests/microbenchmarks/sound-types-add.js and
// --dumpGraphAtEachPhase show the effect.

function shouldBe(actual, expected) {
    if (!Object.is(actual, expected))
        throw new Error(`bad value: ${String(actual)}, expected ${String(expected)}`);
}

function add(a, b) {
    $$t(a, 8);
    $$t(b, 8);
    return a + b;
}
noInline(add);

// Pollute the argument profiles before anything is compiled.
for (let bad of ["s", null, undefined, { }, [], 1n, true, Symbol()]) {
    for (let args of [[bad, 1], [1, bad]]) {
        let threw = false;
        try {
            add(...args);
        } catch (e) {
            threw = e instanceof TypeError;
        }
        shouldBe(threw, true);
    }
}

for (let i = 0; i < testLoopCount; ++i) {
    shouldBe(add(i, 1), i + 1);
    shouldBe(add(i + 0.5, 1), i + 1.5);
    shouldBe(add(0x7fffffff, i), 0x7fffffff + i);
    shouldBe(add(-0, -0), -0);
    shouldBe(add(NaN, i), NaN);
}

// An argument that is reassigned is only bounded until the assignment.
function reassign(a) {
    $$t(a, 8);
    let n = a;
    a = String(a);
    return a + n;
}
noInline(reassign);
for (let i = 0; i < testLoopCount; ++i)
    shouldBe(reassign(i), String(i) + i);

// Contradictory checks: no value gets past both.
function contradiction(a) {
    $$t(a, 8);
    $$t(a, 16);
    return a;
}
noInline(contradiction);
for (let i = 0; i < testLoopCount; ++i) {
    for (let v of [1, "s"]) {
        let threw = false;
        try {
            contradiction(v);
        } catch (e) {
            threw = e instanceof TypeError;
        }
        shouldBe(threw, true);
    }
}

// A check that is not at the top of the function says nothing about the calls that never reach it.
function conditional(a, check) {
    if (check) {
        $$t(a, 8);
        return a + 1;
    }
    return a + "!";
}
noInline(conditional);
for (let i = 0; i < testLoopCount; ++i) {
    shouldBe(conditional(i, true), i + 1);
    shouldBe(conditional("s", false), "s!");
}

// Arguments that are never passed, arguments objects, and this.
function optional(a, b) {
    $$t(a, 8);
    $$t(b, 9);
    return b === undefined ? a : a + b;
}
noInline(optional);
function viaArguments(a) {
    $$t(a, 8);
    arguments[0] = "s";
    return a;
}
noInline(viaArguments);
function onThis() {
    "use strict";
    $$t(this, 8);
    return this + 1;
}
noInline(onThis);
for (let i = 0; i < testLoopCount; ++i) {
    shouldBe(optional(i), i);
    shouldBe(optional(i, 1), i + 1);
    shouldBe(viaArguments(i), "s");
    shouldBe(onThis.call(i), i + 1);
}
