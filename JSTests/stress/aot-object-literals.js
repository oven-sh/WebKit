//@ requireOptions("--evaluateObjectLiteralValuesFirst=1")
// The values of an object literal's properties are worked out first, and the object is made with all of them in it.
function shouldBe(actual, expected) {
    if (!Object.is(actual, expected))
        throw new Error("bad value: " + actual + ", expected " + expected);
}
const show = o => JSON.stringify(o) + "/" + Object.keys(o).join();

function plain(a, b) { return { x: a, y: b, z: 3 }; }
for (let i = 0; i < 100; i++)
    shouldBe(show(plain(i, "s")), `{"x":${i},"y":"s","z":3}/x,y,z`);
shouldBe(Object.getPrototypeOf(plain(1, 2)), Object.prototype);
shouldBe(plain(1, 2) === plain(1, 2), false);
const descriptor = Object.getOwnPropertyDescriptor(plain(1, 2), "y");
shouldBe(descriptor.writable && descriptor.enumerable && descriptor.configurable, true);

// The order things are evaluated in, and what a variable held when its turn came.
function order() { const log = []; const f = n => (log.push(n), n); const o = { a: f(1), b: f(2), c: f(3) }; return log.join() + show(o); }
shouldBe(order(), '1,2,3{"a":1,"b":2,"c":3}/a,b,c');
function snapshot(x) { return { before: x, bump: x++, after: x, again: (x = 10), last: x }; }
shouldBe(show(snapshot(1)), '{"before":1,"bump":1,"after":2,"again":10,"last":10}/before,bump,after,again,last');
function closure() { let x = 1; const set = v => { x = v; return v; }; return { a: x, b: set(5), c: x }; }
shouldBe(show(closure()), '{"a":1,"b":5,"c":5}/a,b,c');

// Something is thrown halfway.
function throws() { let made; try { made = { a: 1, b: (() => { throw 1; })(), c: 2 }; } catch { return made; } }
shouldBe(throws(), undefined);

// The same name twice, names that are not identifiers, and kinds of property that are left alone.
shouldBe(show((v => ({ a: 1, b: 2, a: v }))(3)), '{"a":3,"b":2}/a,b');
shouldBe(show((v => ({ a: v, 1: 2, b: 3 }))(1)), '{"1":2,"a":1,"b":3}/1,a,b');
shouldBe(show((v => ({ a: v, ["k" + v]: 2 }))(1)), '{"a":1,"k1":2}/a,k1');
shouldBe(show((v => ({ a: v, ...{ q: 1 }, b: 2 }))(1)), '{"a":1,"q":1,"b":2}/a,q,b');
shouldBe((v => ({ a: v, get b() { return this.a + 1; } }))(1).b, 2);
shouldBe((v => ({ a: v, m() { return super.toString === Object.prototype.toString; } }))(1).m(), true);
shouldBe(Object.getPrototypeOf((p => ({ a: 1, __proto__: p }))(null)), null);
const __proto__ = 5;
shouldBe(show({ a: 1, __proto__ }), '{"a":1,"__proto__":5}/a,__proto__');
shouldBe(({ f: function () { }, g: () => { }, h: class { } }).f.name + ({ g: () => { } }).g.name + ({ h: class { } }).h.name, "fgh");

// Nested, self-referential by way of a variable, and big.
function nested(v) { return { a: { b: { c: v } }, d: [{ e: v }] }; }
shouldBe(JSON.stringify(nested(1)), '{"a":{"b":{"c":1}},"d":[{"e":1}]}');
function reassigns() { let o = { a: 1 }; o = { prev: o, b: 2 }; return o; }
shouldBe(JSON.stringify(reassigns()), '{"prev":{"a":1},"b":2}');
function big(v) {
    return { p0: v, p1: v, p2: v, p3: v, p4: v, p5: v, p6: v, p7: v, p8: v, p9: v, p10: v, p11: v, p12: v, p13: v, p14: v, p15: v, p16: v, p17: v, p18: v, p19: v,
        p20: v, p21: v, p22: v, p23: v, p24: v, p25: v, p26: v, p27: v, p28: v, p29: v, p30: v, p31: v, p32: v, p33: v, p34: v, p35: v, p36: v, p37: v, p38: v, p39: v,
        p40: v, p41: v, p42: v, p43: v, p44: v, p45: v, p46: v, p47: v, p48: v, p49: v, p50: v, p51: v, p52: v, p53: v, p54: v, p55: v, p56: v, p57: v, p58: v, p59: v,
        p60: v, p61: v, p62: v, p63: v, p64: v, p65: v, p66: v, p67: v, p68: v, p69: v };
}
for (let i = 0; i < 3; i++) {
    shouldBe(Object.keys(big(i)).length, 70);
    shouldBe(Object.values(big(i)).every(v => v === i), true);
}

// In loops, kept and dropped, through collections.
function many(n) { const out = []; for (let i = 0; i < n; i++) out.push({ index: i, half: i / 2, name: "n" + i, self: out }); return out; }
const list = many(20000);
for (let i = 0; i < list.length; i += 997)
    shouldBe(show({ index: list[i].index, half: list[i].half, name: list[i].name }), `{"index":${i},"half":${i / 2},"name":"n${i}"}/index,half,name`);
shouldBe(list[19999].self, list);

// What is done to one afterwards is nothing to the next.
function grows() { const o = { a: 1, b: 2 }; o.c = 3; delete o.a; return o; }
for (let i = 0; i < 10; i++)
    shouldBe(show(grows()), '{"b":2,"c":3}/b,c');
Object.prototype.inherited = 1;
shouldBe(plain(1, 2).inherited, 1);
delete Object.prototype.inherited;
Object.defineProperty(Object.prototype, "x", { set(v) { throw new Error("a setter was called"); }, configurable: true });
shouldBe(plain(7, 2).x, 7);
delete Object.prototype.x;
