//@ skip if $architecture != "arm64"
//@ requireOptions("--compileMainScriptAheadOfTime=1")
//@ defaultRun
//@ run("gc-stress", "--slowPathAllocsBetweenGCs=20")

function inner(value) {
    switch (value + "Statement") {
    case "ExpressionStatement": return 0;
    case "DoWhileStatement": return 5;
    case "ForStatement": return 6;
    default: return 7;
    }
}
function outer(value) {
    switch (value) {
    case "Expression": return 0 + inner(value);
    case "DoWhile": return 5 + inner(value);
    case "For": return 6 + inner(value);
    default: return 7 + inner(value);
    }
}
noInline(outer);

function shouldBe(actual, expected) {
    if (String(actual) !== String(expected))
        throw new Error(`got ${actual}, expected ${expected}`);
}
if (!isAOTCompiled(outer))
    throw new Error("not compiled");

shouldBe([outer("Do" + "While"), outer("F" + "or"), outer(""), outer("TEST"), inner("For"), inner("x")], [10, 12, 14, 14, 6, 7]);
const wide = s => (s + "Ā").slice(0, -1);
shouldBe([outer(wide("DoWhile")), outer(wide("For")), inner(wide("For")), outer(wide("Nope")), outer("FoĀ")], [10, 12, 6, 14, 14]);
