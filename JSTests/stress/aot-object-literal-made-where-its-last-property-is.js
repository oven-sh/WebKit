function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("bad value: " + actual + ", expected " + expected);
}
function show(o) { return JSON.stringify(o, (k, v) => typeof v === "function" ? "fn:" + v.name : typeof v === "bigint" ? v + "n" : v === undefined ? "undef" : v); }

let log = [];
function note(x) { log.push(x); return x; }
function thrower(x) { throw new Error("thrown " + x); }

function order() { log = []; const o = { a: note(1), b: note(2), c: note(3) }; return show(o) + log.join(); }
function closures(x) { return { a: () => x, b: () => x + 1, c: function () { return x + 2; }, d() { return x + 3; } }; }
function nested(x) { return { a: { b: { c: note(x), d: [x] }, e: x + 1 }, f: { g: x + 2 }, h: x + 3 }; }
function sameTemporary(x) { return { a: x + 1, b: x + 2, c: x + 3, d: x * 2, e: String(x), f: -x }; }
function mutation() { let i = 0; return { a: i++, b: i++, c: i, d: (i += 10), e: i }; }
function throwsInTheMiddle() { log = []; try { return { a: note(1), b: thrower(2), c: note(3) }; } catch (e) { return e.message + log.join(); } }
function conditional(c) { return { a: 1, b: c ? 2 : 3, c: c && 4, d: c || 5, e: c ?? 6, f: 7 }; }
function computed(k) { return { a: 1, [k]: 2, b: 3, ["x" + k]: 4, c: 5 }; }
function spread(o) { return { a: 1, ...o, b: 2, ...o, c: 3 }; }
function duplicates(x) { return { a: x, b: x + 1, a: x + 2, c: x + 3, b: x + 4 }; }
function proto(p) { return { a: 1, __proto__: p, b: 2 }; }
function protoAsName(v) { const __proto__ = v; return { a: 1, __proto__, b: 2 }; }
function accessors(x) { return { a: x, get b() { return this.a + 1; }, set b(v) { this.a = v; }, c: x + 2 }; }
function withSuper() { return { __proto__: { m() { return "base"; } }, a: note(1), m() { return "derived " + super.m(); }, b: note(2) }; }
function numeric(x) { return { a: x, 0: x + 1, b: x + 2, 1: x + 3, "2": x + 4 }; }
function selfLike() { let o = { a: 1 }; o = { a: o, b: o.a + 1, c: () => o }; return show(o) + (o.c() === o); }
function inLoop(n) { const all = []; for (let i = 0; i < n; i++) all.push({ i, sq: i * i, s: "v" + i, f: () => i, n: { i } }); return all; }
function inTry(x) { let o; try { o = { a: note(x), b: x > 1 ? thrower(x) : x, c: 3 }; } catch (e) { o = { caught: e.message, x }; } finally { o = { o, done: true }; } return o; }
function keepsIdentity(x) { const a = { x, y: note(x) }, b = { x, y: note(x) }; return a !== b && show(a) === show(b); }
function names() { const o = { a: () => 1, b: function () { }, c: class { }, d: async () => 1, ["e"]: () => 1 }; return [o.a.name, o.b.name, o.c.name, o.d.name, o.e.name].join(); }
function argumentsObject() { return { a: arguments[0], n: arguments.length, b: arguments[1] }; }
function* generator(x) { const o = { a: x, b: yield 1, c: yield 2, d: x + 1 }; return o; }
async function asynchronous(x) { return { a: x, b: await x + 1, c: await Promise.resolve(x + 2), d: x + 3 }; }
function templates(x) { return { a: `v${x}`, b: `${x}${x}`, c: String.raw`\n${x}` }; }
function regexps() { return { a: /a/g, b: /b/i.source, c: /c/.test("c") }; }
function frozen(x) { return Object.freeze({ a: x, b: Object.freeze({ c: note(x) }) }); }
function shapesAgree(x) { const a = { p: note(x), q: x + 1 }, b = { p: 1, q: 2 }; return Object.keys(a).join() === Object.keys(b).join(); }
function addedTo(x) { const o = { a: note(x), b: x }; o.c = 3; o.d = 4; delete o.a; o.a = 5; return o; }

function big(n) {
    let source = "return {";
    for (let i = 0; i < n; i++)
        source += `p${i}: ${i % 3 ? `f(${i})` : i % 2 ? `() => ${i}` : i},`;
    return new Function("f", source + "};");
}

function test() {
    shouldBe(order(), '{"a":1,"b":2,"c":3}1,2,3');
    const c = closures(10); shouldBe([c.a(), c.b(), c.c(), c.d()].join(), "10,11,12,13");
    log = []; shouldBe(show(nested(1)) + log.join(), '{"a":{"b":{"c":1,"d":[1]},"e":2},"f":{"g":3},"h":4}1');
    shouldBe(show(sameTemporary(3)), '{"a":4,"b":5,"c":6,"d":6,"e":"3","f":-3}');
    shouldBe(show(mutation()), '{"a":0,"b":1,"c":2,"d":12,"e":12}');
    shouldBe(throwsInTheMiddle(), "thrown 21");
    shouldBe(show(conditional(0)), '{"a":1,"b":3,"c":0,"d":5,"e":0,"f":7}');
    shouldBe(show(conditional(null)), '{"a":1,"b":3,"c":null,"d":5,"e":6,"f":7}');
    shouldBe(show(conditional("y")), '{"a":1,"b":2,"c":4,"d":"y","e":"y","f":7}');
    shouldBe(show(computed("k")), '{"a":1,"k":2,"b":3,"xk":4,"c":5}');
    shouldBe(show(computed("a")), '{"a":2,"b":3,"xa":4,"c":5}');
    shouldBe(show(spread({ b: 9, z: 8 })), '{"a":1,"b":9,"z":8,"c":3}');
    shouldBe(show(duplicates(1)), '{"a":3,"b":5,"c":4}');
    const p = { inherited: 1 }; const withProto = proto(p);
    shouldBe(Object.getPrototypeOf(withProto) === p && show(withProto) === '{"a":1,"b":2}' && withProto.inherited === 1, true);
    const named = protoAsName(5);
    shouldBe(Object.getPrototypeOf(named) === Object.prototype && Object.keys(named).join() === "a,__proto__,b", true);
    const acc = accessors(1); shouldBe(acc.b, 2); acc.b = 7; shouldBe(show(acc), '{"a":7,"b":8,"c":3}');
    log = []; shouldBe(withSuper().m() + log.join(), "derived base1,2");
    shouldBe(show(numeric(1)), '{"0":2,"1":4,"2":5,"a":1,"b":3}');
    shouldBe(selfLike(), '{"a":{"a":1},"b":2,"c":"fn:c"}true');
    const all = inLoop(50); shouldBe(all[49].sq + all[7].s + all[30].f() + all[12].n.i, "2401v73012");
    log = []; shouldBe(show(inTry(1)), '{"o":{"a":1,"b":1,"c":3},"done":true}');
    shouldBe(show(inTry(2)), '{"o":{"caught":"thrown 2","x":2},"done":true}');
    shouldBe(keepsIdentity(1), true);
    shouldBe(names(), "a,b,c,d,e");
    shouldBe(show(argumentsObject(1, 2, 3)), '{"a":1,"n":3,"b":2}');
    const g = generator(1); g.next(); g.next("first"); shouldBe(show(g.next("second").value), '{"a":1,"b":"first","c":"second","d":2}');
    shouldBe(show(templates(1)), '{"a":"v1","b":"11","c":"\\\\n1"}');
    const r = regexps(); shouldBe(r.a.flags + r.b + r.c, "gbtrue");
    const f = frozen(1); shouldBe(Object.isFrozen(f) && Object.isFrozen(f.b) && f.b.c === 1, true);
    shouldBe(shapesAgree(1), true);
    shouldBe(show(addedTo(1)), '{"b":1,"c":3,"d":4,"a":5}');
    for (const n of [1, 2, 6, 63, 64, 65, 100, 300, 700, 2047, 2048, 2049, 3000]) {
        const make = big(n);
        for (let round = 0; round < 3; round++) {
            const o = make(x => x * 2), keys = Object.keys(o);
            shouldBe(keys.length, n);
            shouldBe(keys[n - 1], "p" + (n - 1));
            for (let i = 0; i < n; i += 7)
                shouldBe(i % 3 ? o["p" + i] : i % 2 ? o["p" + i]() : o["p" + i], i % 3 ? i * 2 : i);
        }
    }
}
for (let i = 0; i < 30; i++)
    test();
asynchronous(1).then(o => shouldBe(show(o), '{"a":1,"b":2,"c":3,"d":4}')).catch(e => { print(e); $vm.abort(); });
