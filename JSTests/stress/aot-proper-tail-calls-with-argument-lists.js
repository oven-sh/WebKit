//@ requireOptions("--compileMainScriptAheadOfTime=1")
"use strict";
// A function that has a rest parameter, uses `arguments`, or has more parameters than there are argument registers is passed its
// arguments as a list in memory. The list has to be somewhere for as long as the callee runs, and a call in tail position
// still must not use up the stack.

function shouldBe(name, actual, expected) {
    if (actual !== expected)
        throw new Error(name + ": " + actual + ", expected " + expected);
}

const N = 300000;
function rest(n, ...more) { return n ? rest(n - 1, 1, 2) : "done " + more.length; }
function restSpread(n, ...more) { return n ? restSpread(n - 1, ...more) : "done " + more.length; }
function tag(strings, ...values) { return values[0] ? tag`a${values[0] - 1}b` : "done"; }
function usesArguments(n) { return n ? usesArguments(n - 1) : "done " + arguments.length; }
function many(n, a, b, c, d, e, f, g, h, i, j, k, l, m) { return n ? many(n - 1, a, b, c, d, e, f, g, h, i, j, k, l, m) : "done " + m; }
function manyPassed(n) { return n ? manyPassed(n - 1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13) : "done"; }
// One kind of list after the other.
function few(n, ...more) { return n ? lots(n - 1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13) : "done few"; }
function lots(n, ...more) { return n ? spread(n - 1, more) : "done lots"; }
function spread(n, list) { return n ? few(n - 1, ...list) : "done spread"; }
// Through a function that takes its arguments in registers.
function inRegisters(n) { return n ? asAList(n - 1, 1) : "done in registers"; }
function asAList(n, ...more) { return n ? inRegisters(n - 1) : "done as a list"; }
const object = { method(n, ...more) { return n ? this.method(n - 1, 1) : "done"; } };
let unknown = function (n, f, ...more) { return n ? f(n - 1, f, 1) : "done"; };

shouldBe("rest", rest(N), "done 2");
shouldBe("rest, spread", restSpread(N, 1, 2, 3), "done 3");
shouldBe("tagged template", tag`a${N}b`, "done");
shouldBe("arguments", usesArguments(N), "done 1");
shouldBe("many parameters", many(N, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13), "done 13");
shouldBe("many arguments", manyPassed(N), "done");
shouldBe("one kind after the other", few(N), "done few");
shouldBe("registers and lists", inRegisters(N), "done in registers");
shouldBe("method", object.method(N), "done");
shouldBe("unknown callee", unknown(N, unknown), "done");

// What is passed arrives, and a call that is not in tail position still returns here.
function sum(...numbers) { let total = 0; for (const number of numbers) total += number; return total; }
function passesOn(a, b, c, d, e, f, g, h, i, j, k, l, m, n) { return sum(a, b, c, d, e, f, g, h, i, j, k, l, m, n); }
function thenAdds(...numbers) { return sum(...numbers) + 1; }
shouldBe("passes on", passesOn(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14), 105);
shouldBe("then adds", thenAdds(1, 2, 3), 7);
// An exception goes past the frames that hold lists.
function throws(n, ...more) { if (!n) throw new Error("thrown"); return throws(n - 1, 1); }
function catches(...more) { try { return throws(1000, 1) + "!"; } catch (e) { return e.message + " " + more.length; } }
shouldBe("exception", catches(1, 2), "thrown 2");
