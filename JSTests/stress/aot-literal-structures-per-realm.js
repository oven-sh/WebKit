//@ skip if $architecture != "arm64"
//@ requireOptions("-m", "--compileMainScriptAheadOfTime=1")
//@ defaultRun
//@ run("gc-stress", "--slowPathAllocsBetweenGCs=20")
// Instances of one realm share the Structures of object literals, which all inherit from the same Object.prototype.

export function literal(i) { return { a: i, b: [i], c: "c" }; }
export function nested(i) { return { outer: { inner: i } }; }
export function spread(o) { return { ...o }; }
export function empty() { return {}; }
export class Point { constructor(x) { this.x = x; } }

const structureID = o => /StructureID: (\d+)/.exec(describe(o))[1];

if (!globalThis.isFurther) {
    globalThis.isFurther = true;
    const own = { literal, nested, spread, empty, Point };
    const all = [own, await importInNewLoader("./aot-literal-structures-per-realm.js"), await importInNewLoader("./aot-literal-structures-per-realm.js")];
    for (let i = 0; i < 100; i++) {
        for (const ns of all) {
            ns.literal(i); ns.nested(i); ns.spread(ns.literal(i)); ns.empty(); new ns.Point(i);
        }
    }
    // (All at once: the identifier of a Structure that has died is used again.)
    const distinct = make => new Set(all.map(make).map(structureID)).size;
    const actual = JSON.stringify([
        distinct(ns => ns.literal(1)), distinct(ns => ns.nested(1)), distinct(ns => ns.nested(1).outer), distinct(ns => ns.spread(ns.literal(1))), distinct(ns => ns.empty()),
        // Each instance has its own Point.prototype.
        distinct(ns => new ns.Point(1)),
        isAOTCompiled(all[1].literal) && isAOTCompiled(all[2].spread),
    ]);
    if (actual !== "[1,1,1,1,1,3,true]")
        throw new Error(`got ${actual}`);
}
