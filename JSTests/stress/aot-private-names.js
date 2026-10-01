//@ requireOptions("--compileMainScriptAheadOfTime=1")
// Private fields, methods and brands, which code from the static compiler has caches for. What is expected is what the interpreter says.

function check(actual, expected) {
    if (actual !== expected)
        throw new Error("bad result:\n" + actual + "\nexpected:\n" + expected);
}

class A {
    #x = 1; #y; static #count = 0;
    constructor(y) { this.#y = y; A.#count++; }
    get x() { return this.#x; }
    sum() { return this.#x + this.#y; }
    bump(n) { this.#x += n; return this; }
    setY(v) { this.#y = v; }
    #secret() { return this.#y * 2; }
    callSecret() { return this.#secret(); }
    static has(o) { return #x in o; }
    static count() { return A.#count; }
    static peek(o) { return o.#x; }
    static poke(o, v) { o.#x = v; }
    static secretOf(o) { return o.#secret(); }
}
class B extends A { #z = 5; extra = 1; z() { return this.#z + this.sum(); } }
let out = [];
const a = new A(2), b = new B(3);
for (let i = 0; i < 50; i++) { out.push(a.sum(), b.sum(), b.z(), a.bump(1).x, a.callSecret(), b.callSecret()); a.setY({ valueOf() { return i; } }); a.setY(i); }
out.push(A.has(a), A.has(b), A.has({}), A.count());
for (const o of [a, b, {}, null, 1, "s"]) {
    for (let i = 0; i < 3; i++) {
        try { out.push(A.peek(o)); } catch (e) { out.push(e.constructor.name + ":" + e.message); }
        try { A.poke(o, 7); out.push("ok"); } catch (e) { out.push(e.constructor.name + ":" + e.message); }
        try { out.push(A.secretOf(o)); } catch (e) { out.push(e.constructor.name + ":" + e.message); }
    }
}
// Two evaluations of the same class body: the same code, other names.
function make() { return class { #p = 0; static get(o) { return o.#p; } static set(o, v) { o.#p = v; } }; }
const C1 = make(), C2 = make(), c1 = new C1(), c2 = new C2();
for (let i = 0; i < 6; i++) {
    C1.set(c1, i); C2.set(c2, -i); out.push(C1.get(c1), C2.get(c2));
    try { out.push(C1.get(c2)); } catch (e) { out.push(e.constructor.name); }
    try { C2.set(c1, 1); out.push("bad"); } catch (e) { out.push(e.constructor.name); }
}
// Objects that grow other properties between accesses.
const grow = new A(1);
for (let i = 0; i < 40; i++) { grow["k" + i] = i; out.push(grow.sum()); grow.bump(1); }
check(out.join(" "), "3 4 9 2 4 6 2 4 9 3 0 6 4 4 9 4 2 6 6 4 9 5 4 6 8 4 9 6 6 6 10 4 9 7 8 6 12 4 9 8 10 6 14 4 9 9 12 6 16 4 9 10 14 6 18 4 9 11 16 6 20 4 9 12 18 6 22 4 9 13 20 6 24 4 9 14 22 6 26 4 9 15 24 6 28 4 9 16 26 6 30 4 9 17 28 6 32 4 9 18 30 6 34 4 9 19 32 6 36 4 9 20 34 6 38 4 9 21 36 6 40 4 9 22 38 6 42 4 9 23 40 6 44 4 9 24 42 6 46 4 9 25 44 6 48 4 9 26 46 6 50 4 9 27 48 6 52 4 9 28 50 6 54 4 9 29 52 6 56 4 9 30 54 6 58 4 9 31 56 6 60 4 9 32 58 6 62 4 9 33 60 6 64 4 9 34 62 6 66 4 9 35 64 6 68 4 9 36 66 6 70 4 9 37 68 6 72 4 9 38 70 6 74 4 9 39 72 6 76 4 9 40 74 6 78 4 9 41 76 6 80 4 9 42 78 6 82 4 9 43 80 6 84 4 9 44 82 6 86 4 9 45 84 6 88 4 9 46 86 6 90 4 9 47 88 6 92 4 9 48 90 6 94 4 9 49 92 6 96 4 9 50 94 6 98 4 9 51 96 6 true true false 2 51 ok 98 7 ok 98 7 ok 98 1 ok 6 7 ok 6 7 ok 6 TypeError:Cannot access invalid private field (evaluating 'o.#x') TypeError:Cannot access invalid private field (evaluating 'o.#x = v') TypeError:Cannot access private method or acessor (evaluating 'o.#secret') TypeError:Cannot access invalid private field (evaluating 'o.#x') TypeError:Cannot access invalid private field (evaluating 'o.#x = v') TypeError:Cannot access private method or acessor (evaluating 'o.#secret') TypeError:Cannot access invalid private field (evaluating 'o.#x') TypeError:Cannot access invalid private field (evaluating 'o.#x = v') TypeError:Cannot access private method or acessor (evaluating 'o.#secret') TypeError:null is not an object (evaluating 'o.#x') TypeError:null is not an object (evaluating 'o.#x = v') TypeError:null is not an object (evaluating 'o.#secret') TypeError:null is not an object (evaluating 'o.#x') TypeError:null is not an object (evaluating 'o.#x = v') TypeError:null is not an object (evaluating 'o.#secret') TypeError:null is not an object (evaluating 'o.#x') TypeError:null is not an object (evaluating 'o.#x = v') TypeError:null is not an object (evaluating 'o.#secret') TypeError:Cannot access invalid private field (evaluating 'o.#x') TypeError:Cannot access invalid private field (evaluating 'o.#x = v') TypeError:Cannot access private method or acessor (evaluating 'o.#secret') TypeError:Cannot access invalid private field (evaluating 'o.#x') TypeError:Cannot access invalid private field (evaluating 'o.#x = v') TypeError:Cannot access private method or acessor (evaluating 'o.#secret') TypeError:Cannot access invalid private field (evaluating 'o.#x') TypeError:Cannot access invalid private field (evaluating 'o.#x = v') TypeError:Cannot access private method or acessor (evaluating 'o.#secret') TypeError:Cannot access invalid private field (evaluating 'o.#x') TypeError:Cannot access invalid private field (evaluating 'o.#x = v') TypeError:Cannot access private method or acessor (evaluating 'o.#secret') TypeError:Cannot access invalid private field (evaluating 'o.#x') TypeError:Cannot access invalid private field (evaluating 'o.#x = v') TypeError:Cannot access private method or acessor (evaluating 'o.#secret') TypeError:Cannot access invalid private field (evaluating 'o.#x') TypeError:Cannot access invalid private field (evaluating 'o.#x = v') TypeError:Cannot access private method or acessor (evaluating 'o.#secret') 0 0 TypeError TypeError 1 -1 TypeError TypeError 2 -2 TypeError TypeError 3 -3 TypeError TypeError 4 -4 TypeError TypeError 5 -5 TypeError TypeError 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40 41");
let s = 0; const hot = new A(4);
for (let i = 0; i < 100000; i++) { s += hot.sum(); hot.setY(i & 7); s += hot.callSecret(); }
check(s, 1149997);
