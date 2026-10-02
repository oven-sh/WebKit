//@ runDefault("--compileMainScriptAheadOfTime=1")
// Objects made by literals whose properties are all plain from the code, and everything that can become of them afterwards.
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function ab(a, b) { return { a, b }; }
function ba(a, b) { return { b, a }; }
function abc(a, b, c) { return { a, b, c }; }
function cab(a, b, c) { return { c, a, b }; }
function xa(x, a) { return { x, a }; }
function many(i) {
    return {
        p0: i, p1: i, p2: i, p3: i, p4: i, p5: i, p6: i, p7: i, p8: i, p9: i, p10: i, p11: i, p12: i, p13: i, p14: i, p15: i, p16: i, p17: i, p18: i, p19: i,
        p20: i, p21: i, p22: i, p23: i, p24: i, p25: i, p26: i, p27: i, p28: i, p29: i, p30: i, p31: i, p32: i, p33: i, p34: i, p35: i, p36: i, p37: i, p38: i, p39: i,
        p40: i, p41: i, p42: i, p43: i, p44: i, p45: i, p46: i, p47: i, p48: i, p49: i, p50: i, p51: i, p52: i, p53: i, p54: i, p55: i, p56: i, p57: i, p58: i, p59: i,
        p60: i, p61: i, p62: i, p63: i, p64: i, p65: i, p66: i, p67: i, p68: i, p69: i, p70: i, p71: i, p72: i, p73: i, p74: i, p75: i, p76: i, p77: i, p78: i, p79: i,
        p80: i, p81: i, p82: i, p83: i, p84: i, p85: i, p86: i, p87: i, p88: i, p89: i, p90: i, p91: i, p92: i, p93: i, p94: i, p95: i, p96: i, p97: i, p98: i, p99: i,
        p100: i + 100, p101: i, p102: i, p103: i, p104: i, p105: i, p106: i, p107: i, p108: i, p109: i, p110: i, p111: i, p112: i, p113: i, p114: i, p115: i, p116: i, p117: i, p118: i, a: i + 119,
    };
}
function a(o) { return o.a; }
function b(o) { return o.b; }
function c(o) { return o.c; }
function p0(o) { return o.p0; }
function p100(o) { return o.p100; }
function toStringOf(o) { return o.toString; }
function keys(o) { let r = []; for (let k in o) r.push(k); return r.join(); }
for (let f of [ab, ba, abc, cab, xa, many, a, b, c, p0, p100, toStringOf, keys])
    noInline(f);

for (let i = 0; i < 3000; ++i) {
    let all = [ab(i, 1), ba(i, 2), abc(i, 3, 4), cab(i, 5, 6), xa(7, i)];
    for (let o of all)
        check(a(o), i, "a");
    check(b(all[0]), 1, "b of ab"); check(b(all[1]), 2, "b of ba"); check(b(all[2]), 3, "b of abc"); check(b(all[3]), 5, "b of cab"); check(b(all[4]), undefined, "b of xa");
    check(c(all[0]), undefined, "c of ab"); check(c(all[2]), 4, "c of abc"); check(c(all[3]), 6, "c of cab");
    check(toStringOf(all[i % 5]), Object.prototype.toString, "inherited");
    let m = many(i);
    check(p0(m), i, "first of many"); check(p100(m), i + 100, "out of line"); check(a(m), i + 119, "last of many"); check(b(m), undefined, "absent from many");
    check(p0(all[0]), undefined, "absent");
}
check(keys(cab(1, 2, 3)), "c,a,b", "order"); check(keys(ba(1, 2)), "b,a", "order");
check(Object.keys(many(0)).length, 120, "how many"); check(JSON.stringify(abc(1, "x", null)), '{"a":1,"b":"x","c":null}', "JSON");

// ---- What was made that way and is no longer what it was.
function variants(i) {
    let added = ab(i, 1); added.c = "added";
    let deleted = abc(i, 1, 2); delete deleted.b;
    let deletedLast = abc(i, 1, 2); delete deletedLast.c;
    let accessor = ab(i, 1); Object.defineProperty(accessor, "a", { get() { return "getter" + i; }, configurable: true });
    let readOnly = ab(i, 1); Object.defineProperty(readOnly, "a", { value: "read only" + i, writable: false });
    let hidden = ab(i, 1); Object.defineProperty(hidden, "b", { enumerable: false });
    let frozen = Object.freeze(ab(i, 1));
    let sealed = Object.seal(ab(i, 1));
    let closed = Object.preventExtensions(ab(i, 1));
    let reparented = Object.setPrototypeOf(ab(i, 1), { c: "from the new prototype" });
    let orphan = Object.setPrototypeOf(ab(i, 1), null);
    let indexed = ab(i, 1); indexed[0] = "element";
    let dictionary = ab(i, 1); for (let j = 0; j < 100; ++j) dictionary["d" + j] = j; for (let j = 0; j < 100; ++j) delete dictionary["d" + j];
    let asPrototype = ab(i, 1); let heir = Object.create(asPrototype);
    let replaced = ab(i, 1); replaced.a = "replaced" + i;
    return { added, deleted, deletedLast, accessor, readOnly, hidden, frozen, sealed, closed, reparented, orphan, indexed, dictionary, asPrototype, heir, replaced };
}
noInline(variants);
for (let i = 0; i < 1500; ++i) {
    let v = variants(i);
    check(c(v.added), "added", "added"); check(a(v.added), i, "added");
    check(b(v.deleted), undefined, "deleted"); check(c(v.deleted), 2, "after the deleted"); check(a(v.deleted), i, "before the deleted");
    check(c(v.deletedLast), undefined, "deleted last"); check(b(v.deletedLast), 1, "deleted last");
    check(a(v.accessor), "getter" + i, "accessor"); check(b(v.accessor), 1, "beside an accessor");
    check(a(v.readOnly), "read only" + i, "read only"); v.readOnly.a = 5; check(a(v.readOnly), "read only" + i, "still read only");
    check(keys(v.hidden), "a", "hidden"); check(b(v.hidden), 1, "hidden");
    check(a(v.frozen), i, "frozen"); v.frozen.a = 5; check(a(v.frozen), i, "still frozen");
    check(a(v.sealed), i, "sealed"); v.sealed.c = 5; check(c(v.sealed), undefined, "sealed");
    v.closed.c = 5; check(c(v.closed), undefined, "closed");
    check(c(v.reparented), "from the new prototype", "another prototype"); check(a(v.reparented), i, "another prototype");
    check(toStringOf(v.orphan), undefined, "no prototype"); check(a(v.orphan), i, "no prototype");
    check(a(v.indexed), i, "indexed"); check(v.indexed[0], "element", "indexed");
    check(a(v.dictionary), i, "dictionary"); check(c(v.dictionary), undefined, "dictionary");
    check(a(v.heir), i, "inherited from a literal"); v.asPrototype.c = "late"; check(c(v.heir), "late", "inherited from a literal, later");
    check(a(v.replaced), "replaced" + i, "replaced");
    if (!(i % 500))
        gc();
}

// ---- What every one of them inherits from cannot be added to, in a program that is compiled ahead of time.
Object.prototype.c = "on Object.prototype";
check(c(ab(1, 2)), undefined, "still absent"); check(c(abc(1, 2, 3)), 3, "own all the same");
{
    let refused = false;
    try { Object.defineProperty(Object.prototype, "b", { get() { return "getter on Object.prototype"; }, configurable: true }); } catch (e) { refused = e instanceof TypeError; }
    check(refused, true, "Object.prototype is not extensible");
}
check(b(xa(1, 2)), undefined, "still absent"); check(b(ab(1, 2)), 2, "own all the same");

// ---- Another realm, with the same code.
if (typeof createGlobalObject === "function") {
    let other = createGlobalObject();
    let source = "(function ab(a, b) { return { a, b }; })";
    let theirs = other.eval(source);
    for (let i = 0; i < 200; ++i) {
        let o = theirs(i, 1);
        check(a(o), i, "another realm's"); check(c(o), undefined, "another realm's");
        check(Object.getPrototypeOf(o) === other.Object.prototype, true, "another realm's prototype");
    }
}

// ---- Objects made by constructors that start by storing to this.
{
    function Point(x, y) { this.x = x; this.y = y; }
    function Reversed(x, y) { this.y = y; this.x = x; }
    class Named { constructor(name, x) { this.name = name; this.x = x; } get shout() { return this.name + "!"; } method() { return this.x; } }
    class Sub extends Named { constructor(name, x, extra) { super(name, x); this.extra = extra; } }
    function x(o) { return o.x; }
    function y(o) { return o.y; }
    function name(o) { return o.name; }
    function extra(o) { return o.extra; }
    function shout(o) { return o.shout; }
    function callMethod(o) { return o.method(); }
    function makeAll(i) { return [new Point(i, 1), new Reversed(i, 2), new Named("n" + i, i), new Sub("s" + i, i, "e" + i), { x: i, y: 3 }]; }
    for (let f of [Point, Reversed, x, y, name, extra, shout, callMethod, makeAll])
        noInline(f);
    for (let i = 0; i < 3000; ++i) {
        let all = makeAll(i);
        for (let o of all)
            check(x(o), i, "x of what was constructed");
        check(y(all[0]), 1, "y"); check(y(all[1]), 2, "y"); check(y(all[2]), undefined, "y"); check(y(all[4]), 3, "y");
        check(name(all[2]), "n" + i, "name"); check(name(all[3]), "s" + i, "name"); check(name(all[0]), undefined, "name");
        check(extra(all[3]), "e" + i, "extra"); check(extra(all[2]), undefined, "extra");
        check(shout(all[2]), "n" + i + "!", "getter"); check(shout(all[3]), "s" + i + "!", "inherited getter"); check(callMethod(all[3]), i, "method");
        if (!(i % 700))
            gc();
    }
    // Classes that come and go, with the same fields.
    function makeClass(tag) { return class { constructor(a, b) { this.x = a; this.y = b; } tag() { return tag; } }; }
    noInline(makeClass);
    for (let i = 0; i < 400; ++i) {
        let C = makeClass(i);
        let o = new C(i, i + 1);
        check(x(o), i, "x of a passing class"); check(y(o), i + 1, "y of a passing class"); check(o.tag(), i, "tag");
        if (!(i % 50))
            gc();
    }
    // The prototype gets a say.
    Object.defineProperty(Point.prototype, "y", { set(v) { this.hidden = v; }, get() { return "accessor"; }, configurable: true });
    for (let i = 0; i < 300; ++i) {
        let p = new Point(i, 5);
        check(y(p), "accessor", "a setter on the prototype"); check(x(p), i, "beside it"); check(p.hidden, 5, "what the setter did");
    }
}

// ---- Stores to what such objects have, and to what they do not.
{
    function setA(o, v) { o.a = v; }
    function setC(o, v) { o.c = v; }
    function setP100(o, v) { o.p100 = v; }
    function strictSetA(o, v) { "use strict"; o.a = v; }
    for (let f of [setA, setC, setP100, strictSetA])
        noInline(f);
    let kept = [];
    for (let i = 0; i < 3000; ++i) {
        let all = [ab(0, 1), ba(0, 2), abc(0, 3, 4), cab(0, 5, 6), xa(7, 0)];
        for (let o of all) {
            setA(o, i); check(a(o), i, "stored");
            setA(o, { boxed: i }); check(a(o).boxed, i, "stored a cell");
        }
        setC(all[2], "c" + i); check(c(all[2]), "c" + i, "stored c"); check(b(all[2]), 3, "beside it");
        setC(all[0], "new" + i); check(c(all[0]), "new" + i, "added by a store"); check(keys(all[0]), "a,b,c", "added last");
        let m = many(i); setP100(m, "far"); check(p100(m), "far", "stored out of line"); check(p0(m), i, "far from it");
        if (!(i % 300)) { kept.push(...all); gc(); }
    }
    for (let o of kept)
        check(typeof a(o).boxed, "number", "what was stored is still there");
    let frozen = Object.freeze(ab(1, 2)); setA(frozen, 5); check(a(frozen), 1, "frozen");
    let threw = false; try { strictSetA(frozen, 5); } catch (e) { threw = e instanceof TypeError; } check(threw, true, "frozen, strict");
    let readOnly = Object.defineProperty(ab(1, 2), "a", { writable: false }); setA(readOnly, 5); check(a(readOnly), 1, "read only");
    let seen; let withSetter = Object.defineProperty(ab(1, 2), "a", { set(v) { seen = v; }, configurable: true }); setA(withSetter, 9); check(seen, 9, "a setter");
    let plain = ab(1, 2); setC(plain, 3); check(c(plain), 3, "a new property"); check(Object.hasOwn(plain, "c"), true, "of its own");
    let inheritsSetter = Object.setPrototypeOf(ab(1, 2), { set c(v) { seen = "prototype " + v; } }); setC(inheritsSetter, 3); check(seen, "prototype 3", "a setter to inherit"); check(Object.hasOwn(inheritsSetter, "c"), false, "and nothing added");
    let own = abc(1, 2, 3); setC(own, 4); check(c(own), 4, "own all the same");
}
