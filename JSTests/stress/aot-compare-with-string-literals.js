//@ skip if $architecture != "arm64"
//@ requireOptions("--compileMainScriptAheadOfTime=1")
//@ defaultRun
//@ run("gc-stress", "--slowPathAllocsBetweenGCs=20")

// Nothing but comparisons says these literals, so no string is ever made of them.
function kindOf(value) {
    if (value === "alpha")
        return 1;
    if (value === "alphabet")
        return 2;
    if ("a-literal-of-more-than-sixteen-characters" === value)
        return 3;
    if (value == "beta")
        return 4;
    if (value !== "gamma")
        return 0;
    return 5;
}
noInline(kindOf);
function inLoop(values) {
    let sum = 0;
    for (let i = 0; i < values.length; i++) {
        if (values[i] === "alpha")
            sum += 1;
        else if (values[i] === "delta")
            sum += 10;
    }
    return sum;
}
noInline(inLoop);
// These are made of the literals.
function literals() { return ["alpha", "alphabet", "beta", "gamma", "delta"]; }

function shouldBe(actual, expected) {
    if (String(actual) !== String(expected))
        throw new Error(`got ${actual}, expected ${expected}`);
}
if (!isAOTCompiled(kindOf) || !isAOTCompiled(inLoop))
    throw new Error("not compiled");

const wide = s => (s + "Ā").slice(0, -1);
const rope = s => s.slice(0, 2) + s.slice(2);
const built = s => Array.from(s).join("");
for (let round = 0; round < 3; round++) {
    for (const make of [s => s, wide, rope, built, s => ("x" + s + "y").slice(1, -1), s => rope(wide(s))]) {
        shouldBe(["alpha", "alphabet", "a-literal-of-more-than-sixteen-characters", "beta", "gamma", "alphb", "alph", "", "gammĀ"].map(s => kindOf(make(s))), [1, 2, 3, 4, 5, 0, 0, 0, 0]);
        shouldBe(inLoop(["alpha", "delta", "alphb", "alpha"].map(make)), 12);
    }
    // A substring that shares the characters of a longer literal, from its start and from its middle.
    shouldBe([kindOf("alphabet".substring(0, 5)), kindOf("an alphabet".substring(3)), kindOf("an alphabet".substring(3, 8))], [1, 2, 1]);
    shouldBe([undefined, null, 5, 5.5, true, {}, Symbol.iterator, 5n].map(kindOf), [0, 0, 0, 0, 0, 0, 0, 0]);
    shouldBe(kindOf({ toString() { return "beta"; } }), 4);
    // From the second round on, strings have been made of some of the literals.
    shouldBe(literals().map(kindOf), [1, 2, 4, 5, 0]);
    shouldBe(inLoop(literals()), 11);
}
