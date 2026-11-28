//@ requireOptions("--maxPerThreadStackUsage=524288")
"use strict";

// An OSR exit into an inlined tail call leaves the frame of the caller on the stack.
const callsToOverflow = 6000;
const calls = (testLoopCount >> 1) + callsToOverflow;

function thrower() { throw null; }
noInline(thrower);

function inCatch(n) { if (!n) return "done"; try { throw null; } catch { return inCatch(n - 1); } }
function inFinally(n) { if (!n) return "done"; try { throw null; } finally { return inFinally(n - 1); } }
function afterTry(n) { if (!n) return "done"; try { throw null; } catch { } return afterTry(n - 1); }

function mutual(n) { if (!n) return "done"; try { throw null; } catch { return mutualOther(n); } }
function mutualOther(n) { try { throw null; } catch { return mutual(n - 1); } }

function nested(n) { if (!n) return "done"; return nestedTail(n); }
function nestedTail(n) { nestedCatch(); return nested(n - 1); }
function nestedCatch() { try { throw null; } catch { } }

function twoTails(n) { if (!n) return "done"; return twoTailsFirst(n); }
function twoTailsFirst(n) { return twoTailsSecond(n); }
function twoTailsSecond(n) { try { thrower(); } catch { } return twoTails(n - 1); }

function late(n) { if (!n) return "done"; return lateTail(n); }
function lateTail(n) { try { lateCallee(n); } catch { } return late(n - 1); }
function lateCallee(n) { if (n < callsToOverflow) throw null; }
noInline(lateCallee);

function operation(n) { if (!n) return "done"; return operationTail(n, { valueOf: thrower }); }
function operationTail(n, value) { try { value * 2; } catch { } return operation(n - 1); }

function varargs(n) { if (!n) return "done"; return varargsTail.apply(null, [n]); }
function varargsTail(n) { try { thrower(); } catch { } return varargs(n - 1); }

function loop(n) { if (!n) return "done"; for (let i = 0; i < 20; i++) n |= 0; return loopTail(n); }
function loopTail(n) { try { thrower(); } catch { } return loop(n - 1); }

for (const test of [inCatch, inFinally, afterTry, mutual, nested, twoTails, late, operation, varargs, loop]) {
    const result = test(calls);
    if (result !== "done")
        throw new Error(`${test.name}: ${result}`);
}

// Where the stack does not grow the code is kept.
function wrapper(n) { return wrapped(n); }
function wrapped(n) { try { thrower(); } catch { return Object(n); } }
noInline(wrapper);
for (let i = 0; i < testLoopCount; i++)
    wrapper(null);
if (!jscOptions().useConcurrentJIT && reoptimizationRetryCount(wrapper))
    throw new Error(`reoptimized ${reoptimizationRetryCount(wrapper)} times`);

// Many arguments look the same. The outer frames exit after the code is thrown away.
const many = new Array(3000).fill(0);
function deep(n) { return deepTail(n); }
function deepTail(n) { try { deepCallee(n); } catch (error) { throw error + 1; } }
function deepCallee(n) { if (n === 1) throw 0; if (n) deep(n - 1, ...many); }
noInline(deep);
noInline(deepCallee);
for (let i = 0; i < testLoopCount; i++)
    deep(0);
let caught;
try { deep(4); } catch (error) { caught = error; }
if (caught !== 4)
    throw new Error(`caught ${caught}`);
