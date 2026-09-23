// `current`, `entered` and `collect` are bindings of the loader's: functions of the test's, which belong to no owner.
// current(): which owner is the current one. entered(): how many functions on the stack made theirs the current one
// when they were called and have the previous one to put back.

// What a function returns comes back through whatever puts the previous owner back.
export function value(x) { return x; }
export function valueAfterCall(x) { current(); return x; }
export function state() { return [current(), entered()]; }

// A call with fewer or more arguments than parameters: the frame is not where the caller put it.
export function fewer(a, b, c, d, e, f) { return [current(), entered(), a, b, c, d, e, f, arguments.length]; }
export function more() { return [current(), entered(), arguments.length, arguments[arguments.length - 1]]; }

// Calls that are not in tail position (the result is used after the call).
export function calls(f, ...args) { const result = f(...args); return [result, current()][0]; }
export function callsFixed(f, a, b) { const result = f(a, b); return [result, current()][0]; }

// Calls in tail position: the frame is given to the callee, with where it returns to.
export function tail(f, ...args) { return f(...args); }
export function tailFixed(f, a, b) { return f(a, b); }
export function tailFewer(f) { return f(1); }
export function tailMore(f) { return f(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12); }
// A chain of tail calls from one owner's function to another's and back: every hop is in the one frame.
export function hop(n, mine, theirs, seen) {
    seen.push(current());
    if (!n)
        return [current(), entered()];
    return theirs.hop(n - 1, theirs, mine, seen);
}

// Exceptions.
export function throws(what) { throw what; }
export function throwsAfterCalling(f, what) { f(); throw what; }
export function catchesOwn(f) { try { f(); return "did not throw"; } catch (error) { return [current(), entered(), error]; } }
export function finallyRuns(f, log) { try { return f(); } finally { log.push([current(), entered()]); } }
export function tailThrows(what) { return throws(what); }
// As deep as the stack lets it go, one owner's function calling the other's.
export function overflow(mine, theirs, deepest) { deepest.depth = entered(); return [theirs.overflow(theirs, mine, deepest)]; }

// A collection while functions that entered are on the stack.
export function collects(f) { collect(); const result = f ? f() : entered(); collect(); return [result, current()][0]; }

// What native code calls: a comparator, toJSON, a replacer, an iterator, a getter, a proxy's trap, a constructor.
export function comparator(a, b) { comparator.seen.push(current() + entered()); return a - b; }
comparator.seen = [];
export const json = { toJSON() { return [current(), entered()]; } };
export function replacer(match) { return current() + entered(); }
export const iterable = { *[Symbol.iterator]() { yield [current(), entered()]; yield [current(), entered()]; } };
export const accessors = { get prop() { return [current(), entered()]; } };
export const trapped = new Proxy({}, { get() { return [current(), entered()]; }, has() { return current() === "A" || current() === "B"; } });
export class Made { constructor(x) { this.state = [current(), entered()]; this.x = x; this.newTarget = new.target.name; } }
export class MadeDerived extends Made { constructor(x) { super(x); this.derivedState = [current(), entered()]; } }
export function returnsObjectFromConstructor(x) { this.ignored = true; return { replaced: x, state: [current(), entered()] }; }

// Generators and async functions: every resumption is a call of the function.
export function* generator() { yield [current(), entered()]; yield [current(), entered()]; return [current(), entered()]; }
export async function asyncFunction(f) { const before = [current(), entered()]; await null; if (f) f(); return [before, [current(), entered()]]; }

// A loop hot enough to be entered by OSR while the function, which was called from outside, is running.
export function hotLoop(n) {
    let sum = 0;
    for (let i = 0; i < n; ++i)
        sum += i & 1;
    return [current(), entered(), sum];
}
// Optimized for one kind of value, then given another: exits to the tier below in the frame that entered.
export function reads(object) {
    let sum = 0;
    for (let i = 0; i < 50; ++i)
        sum += object.v;
    return [current(), entered(), sum];
}
// Calls within the owner, which the optimizing tiers inline.
function inner(x) { return x + (current() === "none" ? 1000000 : 1); }
export function callsInside(n) { let sum = 0; for (let i = 0; i < n; ++i) sum = inner(sum); return [current(), entered(), sum]; }

// Called by WebAssembly.
export function fromWasm() { return new Error("from wasm").stack.includes("fromWasm") && current() !== "none" ? entered() : -1; }
export function throwsFromWasm() { throw new Error("thrown as " + current() + entered()); }
