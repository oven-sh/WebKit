function assert(cond, msg) {
    if (!cond)
        throw new Error("FAIL: " + msg);
}

function throwsTypeError(f) {
    try {
        f();
    } catch (e) {
        return e instanceof TypeError;
    }
    return false;
}

Object.freeze(Object.prototype);

function assignOne(t, s) { return Object.assign(t, s); }
function assignTwo(t, s1, s2) { return Object.assign(t, s1, s2); }

for (let i = 0; i < 1e4; i++) {
    const r = assignOne({}, { a: i, b: 2 });
    assert(r.a === i && r.b === 2, "plain assign");
}

for (let i = 0; i < 1e4; i++) {
    const t = {};
    assert(throwsTypeError(() => assignOne(t, { a: 1, toString: 2, b: 3 })), "colliding key throws");
    assert(t.a === 1 && !Object.hasOwn(t, "toString") && !Object.hasOwn(t, "b"), "keys before collision assigned, after not");
}

for (let i = 0; i < 1e4; i++) {
    const t = {};
    assert(throwsTypeError(() => assignTwo(t, { a: 1 }, { constructor: 2 })), "multi-source collision throws");
    assert(t.a === 1 && !Object.hasOwn(t, "constructor"), "first source assigned");
    const r = assignTwo({}, { a: 1 }, { b: 2 });
    assert(r.a === 1 && r.b === 2, "multi-source plain");
}

{
    let setterCalls = 0;
    const src = { x: 1, y: 2 };
    const proto = Object.freeze(Object.create(Object.prototype, {
        x: { set(v) { setterCalls++; this._x = v; src.y = 99; }, get() { return this._x; } },
    }));
    for (let i = 0; i < 1e4; i++) {
        src.y = 2;
        const t = Object.create(proto);
        assignOne(t, src);
        assert(t._x === 1 && t.y === 99 && !Object.hasOwn(t, "x"), "setter invoked and later key re-read");
    }
    assert(setterCalls === 1e4, "setter call count");
}

for (let i = 0; i < 1e4; i++) {
    const r = assignOne({}, JSON.parse('{"__proto__": {"polluted": 1}, "k": 1}'));
    assert(r.k === 1, "json source with __proto__ key");
    assert(!Object.hasOwn(r, "__proto__") && r.polluted === 1, "__proto__ key goes through the Object.prototype setter");
}

{
    const r = Object.assign({}, { a: 1 }, [7, 8]);
    assert(r.a === 1 && r[0] === 7 && r[1] === 8, "indexed source");
}
