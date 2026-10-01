//@ skip if $architecture != "arm64"
//@ requireOptions("--compileMainScriptAheadOfTime=1")
// If the image were not used, every other test of ahead-of-time compiled code would still pass, in the interpreter.

function plain() { return 1; }
const arrow = () => 2;
class C { method() { return 3; } }
function outer() { return function inner() { return 4; }; }
function* generator() { yield 5; }
async function asyncFunction() { return 6; }

const functions = [plain, arrow, C.prototype.method, outer, outer(), generator, asyncFunction];
for (const f of functions)
    f();

for (const f of functions) {
    if (!isAOTCompiled(f))
        throw new Error(`${f.name} should run ahead-of-time compiled code`);
}

// The compiler never saw this one.
const evaluated = (0, eval)("(function evaluated() { return 7; })");
evaluated();
if (isAOTCompiled(evaluated))
    throw new Error("code from eval cannot have been compiled ahead of time");
