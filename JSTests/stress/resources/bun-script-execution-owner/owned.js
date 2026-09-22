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
