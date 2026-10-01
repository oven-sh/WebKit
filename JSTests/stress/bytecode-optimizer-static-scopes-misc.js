//@ runDefault("--useBytecodeOptimizer=1", "--validateBytecodeOptimizerStaticScopes=1")
//@ runDefault("--useBytecodeOptimizer=1", "--validateBytecodeOptimizerStaticScopes=1", "--useConcurrentJIT=0", "--jitPolicyScale=0")
// Static scope resolution / scope caching / TDZ-check elimination must not change which binding a nested function
// sees. The validator turns any disagreement with JSScope::abstractResolve into a crash; the expectations below pin the
// value semantics (recorded from an unoptimized run). Each section was an attack on the DeclaredNamesLink model:
// sloppy `with (o) if (1) function f() {}`: if-statement function declarations directly under with (no block)
// call vs construct code blocks of the same function: only the first generated specialization receives the parent DeclaredNamesLink (takeParentDeclaredNames), both must stay correct
function shouldBe(actual, expected, what) {
    if (String(actual) !== String(expected))
        throw new Error(what + ": expected " + JSON.stringify(String(expected)) + " but got " + JSON.stringify(String(actual)));
}
let __expected, __idx, __what;
function section(what, expected) { if (__expected && __idx !== __expected.length) throw new Error(__what + ": only " + __idx + " of " + __expected.length + " checks ran"); __what = what; __expected = expected; __idx = 0; }
function print(...args) { const v = args.map(String).join(" "); for (const line of v.split("\n")) shouldBe(line, __expected[__idx++], __what + " #" + __idx); }

// --- sloppy `with (o) if (1) function f() {}`: if-statement function declarations directly under with (no block)
section("22-with-nobrace.js", ["W5 kx", "W6y kxy", "W7 kx", "WP"]);
{
function k(o) { let x = "kx"; with (o) if (1) function f5() { return () => x; } return f5()(); }
print(k({x: "W5"}), k({}));
function k2(o) { let x = "kx"; with (o) if (0) ; else function f6() { let y = "y"; return () => [x, y].join(""); } return f6()(); }
print(k2({x: "W6"}), k2({}));
function k3(o) { let x = "kx"; for (const i of [1]) with (o) if (1) function f7() { return () => x; } return f7()(); }
print(k3({x: "W7"}), k3({}));
// program level
var px = "px";
with ({px: "WP"}) if (1) function f8() { return () => px; }
print(f8()());

}

// --- call vs construct code blocks of the same function: only the first generated specialization receives the parent DeclaredNamesLink (takeParentDeclaredNames), both must stay correct
section("23-construct-vs-call.js", ["oxnew,oxcall,oxnew", "oxcall,oxnew,oxcall"]);
{
function outer() { let x = "ox"; function F() { if (!new.target) return (() => x + "call")(); this.v = (() => x + "new")(); } return [new F().v, F(), new F().v].join(","); }
print(outer());
function outer2() { let x = "ox"; function F() { if (!new.target) return (() => x + "call")(); this.v = (() => x + "new")(); } return [F(), new F().v, F()].join(","); }
print(outer2());

}

section("end", []);
