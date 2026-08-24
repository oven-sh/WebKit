//@ runDefault
//@ runDefault("--collectContinuously=1")
//@ runDefault("--useSourceProviderCache=0")

// What generateProgramBytecode()/generateModuleBytecode() write must be a function of the source alone: embedders build
// the cache on one platform and load it on another, and check that the two agree by comparing what each encodes.
// Hash-table order, heap addresses or padding bytes showing up in the payload break that; within one process they show
// up as two encodings of the same source differing. (What must also hold, but a single process cannot check, is that the
// three configurations above encode the same bytes: whether the parser skipped a nested function's body through the
// SourceProviderCache, and whether a collection emptied that cache part-way, must not show in the payload either.)

const corpus = String.raw`
function classify(n, s) {
    let a;
    switch (n) { case 1: a = "one"; break; case 2: a = "two"; break; case 3: a = "three"; break; case 4: a = "four"; break; default: a = "many"; }
    switch (s) {
    case "alpha": return a + ":a";
    case "beta": return a + ":b";
    case "gamma": return a + ":g";
    case "delta": return a + ":d";
    case "epsilon": return a + ":e";
    case "zeta": return a + ":z";
    default: return a + ":?";
    }
}
function literals() {
    const doubles = [1.5, 2.25, -0.5, 1e21];
    const mixed = ["x", 7, null, true];
    const re = /(\d+)-(\w+)/gu;
    const big = 12345678901234567890123n * -3n;
    const tag = (s, ...v) => s.raw.join("|") + "#" + v.join(",");
    let caught;
    try { null.x; } catch (e) { caught = e.constructor.name; } finally { mixed.push("f"); }
    return [doubles, mixed, "ab-2020".replace(re, "$2/$1"), big, tag${"`"}a${"$"}{1}b${"$"}{2}c${"`"}, caught];
}
class Base {
    #secret = 41;
    static #count = 0;
    static make(v) { Base.#count++; return new Base(v); }
    constructor(v) { this.v = v; }
    get secret() { return this.#secret; }
    #bump() { return ++this.#secret; }
    describe() { this.#bump(); return "Base(" + this.v + ")"; }
    static tally() { return Base.#count; }
    static isBase(o) { return #secret in o; }
}
class Derived extends Base {
    publicField = 42;
    ["computed" + 1] = 1;
    static ["computedStatic" + 2] = 2;
    constructor(a, b) { super(a); this.b = b; }
    describe() { return "Derived(" + super.describe() + ", " + this.b + ", " + this.secret + ")"; }
}
function* gen(n) { for (let i = 0; i < n; i++) yield i * 11; }
async function asyncStuff(n, ...rest) {
    const seen = [];
    for await (const v of (async function*() { yield* rest; })()) seen.push(v);
    for (const v of gen(n)) seen.push(v);
    label: for (const k in { p: 1, q: 2, r: 3 }) { if (k === "r") break label; seen.push(k); }
    return seen;
}
function skippedBodies() {
    const spansLines = (a, b) =>
        a +
            b;
    function inner() { const t = (s, ...v) => s.raw.join("|") + v; return t${"`"}x${"$"}{spansLines(1, 2)}
y${"`"}; }
    return [1, 2, 3].map(v =>
        v * spansLines(v, 1)).concat(inner());
}
function tdz() {
    const fns = [];
    for (let i = 0; i < 3; i++) { let captured = i; fns.push(() => captured + i); }
    { let x = 1; { let x = 2; fns.push(() => x); } fns.push(() => x); }
    return fns.map(f => f());
}
function sloppy() {
    var o = { a: { b: null } };
    with (o) { a.c = 1; }
    return [o?.a?.b?.c ?? "dflt", 2 ** 10, delete o.a, "a" in o, typeof o.zz, void 0, arguments.length];
}
`;

function mustMatch(kind, a, b) {
    if (a.length !== b.length)
        throw new Error(kind + ": encodings differ in length, " + a.length + " vs " + b.length);
    for (let i = 0; i < a.length; i++) {
        if (a[i] !== b[i])
            throw new Error(kind + ": encodings differ at byte " + i + " of " + a.length);
    }
}

for (const kind of ["program", "module"]) {
    const source = kind === "module" ? corpus.replace("with (o) { a.c = 1; }", "") + "\nexport { classify, Derived };\nexport default await asyncStuff(2);\n" : corpus;
    const first = bytecodeCacheFor(source, kind);
    const second = bytecodeCacheFor(source, kind);
    if (!first.length)
        throw new Error(kind + ": empty encoding");
    mustMatch(kind, first, second);
}
