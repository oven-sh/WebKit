// `current` is a binding of the loader's: a function of the test's, which belongs to no owner and says which one is current.
export function plain() { return current(); }
export const arrow = () => current();
export function withArguments(a, b) { return [current(), a, b, arguments.length, this === undefined ? "undefined" : this.tag]; }
export function withRest(...rest) { return [current(), rest.length, rest[rest.length - 1]]; }
export function withDefault(a = current()) { return [a, current()]; }
export class Thing {
    constructor(x) { this.madeAs = current(); this.x = x; this.newTargetName = new.target.name; }
    method() { return current(); }
    get prop() { return current(); }
    set prop(value) { this.setAs = current(); }
    static make() { return current(); }
    #secret() { return current(); }
    viaPrivate() { return this.#secret(); }
    field = current();
}
export class Derived extends Thing {
    constructor(x) { super(x); this.derivedAs = current(); }
}
export function* gen() { yield current(); yield current(); return current(); }
export async function asyncFunction() { const before = current(); await null; return [before, current()]; }
export async function* asyncGen() { yield current(); yield current(); }
export const iterable = { *[Symbol.iterator]() { yield current(); yield current(); } };
export const thenable = { then(resolve) { resolve(current()); } };
export const coercible = { toString() { return current(); }, [Symbol.toPrimitive]() { return current(); }, toJSON() { return current(); } };
export const trapped = new Proxy({}, { get() { return current(); } });
export function makeClosure() { return () => current(); }
export const bound = plain.bind(null);
export function thrower() { throw new Error("thrown as " + current()); }
export function callsBack(callback) { return [current(), callback(), current()]; }
export function stackOf() { return new Error("trace").stack; }
export function made() { return new Function("return 1")() + 0 === 1; }

// Entered from outside while it runs a loop hot enough for every tier: its own code runs once, as its owner.
let runs = 0;
function inner(x) { return x + 1; }
export function hot(n) {
    const atStart = current();
    ++runs;
    let sum = 0;
    for (let i = 0; i < n; ++i)
        sum = inner(sum);
    return atStart + ">" + current() + ":" + sum;
}
export function hotRuns() { return runs; }
export function callsInner(n) { let sum = 0; for (let i = 0; i < n; ++i) sum = inner(sum); return sum; }

// A tail call of a function with the same code (which the FTL makes a jump back to the top) reaches another owner's.
export function tail(n, other) { if (n === 0) return current(); return other(n - 1, other); }
export function tailTurns(n, mine, theirs) { if (n === 0) return current(); return theirs(n - 1, theirs, mine); }
export function countTailsNotRunningAs(name, mine, theirs, rounds) {
    let wrong = 0;
    for (let i = 0; i < rounds; ++i) {
        if (mine(1, theirs) !== name)
            ++wrong;
    }
    return wrong;
}
export function countTailTurnsNotRunningAs(name, mine, theirs, rounds) {
    let wrong = 0;
    for (let i = 0; i < rounds; ++i) {
        if (mine(3, mine, theirs) !== name)
            ++wrong;
    }
    return wrong;
}

// A small function its own owner's loop calls, which is then called from outside too.
export function increment(x) { return x + 1; }
export function label() { return "label"; }
export function incrementLoop(n) { let sum = 0; for (let i = 0; i < n; ++i) sum = increment(sum); return sum; }

// How deep two functions can call each other.
export function descend(n, mine, theirs) {
    try {
        return theirs(n + 1, theirs, mine);
    } catch (error) {
        if (error instanceof RangeError)
            return n;
        throw error;
    }
}

// A loop in one owner's function around a call of another's, whose result it goes on computing with.
export function accumulate(other, n, start) { let sum = start; for (let i = 0; i < n; ++i) sum = other(sum); return sum; }
export function half(x) { return x + 0.5; }
export function wrap(x) { return { value: (x ? x.value : 0) + 1 }; }

// What a stack trace shows of calls that enter owners.
export function tailThrower() { return thrower(); }
export function relay(next, last) { const result = next ? next(last) : last(); return result; }

// Hot calls, inside an owner's code, of another function of the same module, that the DFG inlines where it cannot exit
// (a varargs call has loaded its arguments by then): the callee's owner is checked in the callee's frame instead.
const add = (a, b) => (a | 0) + (b | 0);
class Sum { constructor(a, b) { this.sum = (a | 0) + (b | 0); } }
class SumOfSpread extends Sum { constructor(...args) { super(...args); } }
class SumOfArguments extends Sum { constructor() { super(...arguments); } }
export const varargsShapes = {
    spread(n) { const f = (...a) => add(...a); let s = 0; for (let i = 0; i < n; ++i) s += f(i, 1); return s; },
    applyArguments(n) { function f() { return add.apply(null, arguments); } let s = 0; for (let i = 0; i < n; ++i) s += f(i, 1); return s; },
    applyArray(n) { const array = [1, 2]; let s = 0; for (let i = 0; i < n; ++i) s += add.apply(null, array); return s; },
    newSpread(n) { const array = [1, 2]; let s = 0; for (let i = 0; i < n; ++i) s += new Sum(...array).sum; return s; },
    reflectApply(n) { let s = 0; for (let i = 0; i < n; ++i) s += Reflect.apply(add, null, [i, 1]); return s; },
    forwardArguments(n) { function g() { return add(arguments[0], arguments[1]); } function f() { return g.apply(this, arguments); } let s = 0; for (let i = 0; i < n; ++i) s += f(i, 1); return s; },
    superSpread(n) { let s = 0; for (let i = 0; i < n; ++i) s += new SumOfSpread(i, 1).sum; return s; },
    superSpreadArguments(n) { let s = 0; for (let i = 0; i < n; ++i) s += new SumOfArguments(i, 1).sum; return s; },
    proxyApplyTrap(n) { const proxy = new Proxy(add, { apply: (target, thisValue, args) => target(...args) }); let s = 0; for (let i = 0; i < n; ++i) s += proxy(i, 1); return s; },
    proxyConstructTrap(n) { const proxy = new Proxy(Sum, { construct: (target, args) => new target(...args) }); let s = 0; for (let i = 0; i < n; ++i) s += new proxy(i, 1).sum; return s; },
    callSpreadArguments(n) { function g() { return add.call(null, ...arguments); } let s = 0; for (let i = 0; i < n; ++i) s += g(i, 1); return s; },
    applyRest(n) { const f = (...rest) => add.apply(null, rest); let s = 0; for (let i = 0; i < n; ++i) s += f(i, 1); return s; },
    crossingSpread(n, other) { const array = [1, 2]; let s = 0; for (let i = 0; i < n; ++i) s += other(...array); return s; },
    current() { return current(); },
};
export function addOf(a, b) { return add(a, b); }
