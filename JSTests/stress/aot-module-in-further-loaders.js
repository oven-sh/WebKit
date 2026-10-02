//@ skip if $architecture != "arm64"
//@ requireOptions("-m", "--compileMainScriptAheadOfTime=1")
//@ defaultRun
//@ run("gc-stress", "--slowPathAllocsBetweenGCs=20")

let n = 0;
export function count() { return ++n; }
export const viaClosure = () => () => count();
export function call(f) { return f() * 100 + count(); }
export function* each() { while (true) yield count(); }
export async function later() { await 1; return count(); }
export class Counter { get next() { return count(); } }
export class Empty { }
export class Derived extends Empty { field = count(); }
export const matches = text => /^(a+)b$/.exec(text)[1].length + n;

if (!globalThis.isFurther) {
    globalThis.isFurther = true;
    const a = await importInNewLoader("./aot-module-in-further-loaders.js");
    const b = await importInNewLoader("./aot-module-in-further-loaders.js");
    const actual = JSON.stringify([
        a.count(), a.count(), count(), b.count(), a.count === b.count, a.count === count,
        call(a.count), a.call(b.count), b.call(count), a.viaClosure()(), b.each().next().value, await a.later(),
        new a.Counter().next, new b.Derived().field, new Derived().field, new a.Empty() instanceof Empty, a.matches("aaab"), matches("ab"),
        [count, a.count, b.count, a.call, b.each, a.later, a.matches].map(isAOTCompiled),
    ]);
    const expected = JSON.stringify([
        1, 2, 1, 1, false, false,
        302, 204, 303, 5, 4, 6,
        7, 5, 4, false, 10, 5,
        [true, true, true, true, true, true, true],
    ]);
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}
