// Object.seal / Object.freeze fast path on JSArray and JSFinalObject with
// indexed properties should produce the same observable state as the generic
// SetIntegrityLevel loop.

function shouldBe(actual, expected, msg) {
    if (actual !== expected)
        throw new Error((msg ? msg + ": " : "") + "expected " + expected + ", got " + actual);
}
function shouldThrow(fn, msg) {
    let threw = false;
    try { fn(); } catch { threw = true; }
    if (!threw)
        throw new Error((msg ? msg + ": " : "") + "expected throw");
}
function desc(o, k) { return Object.getOwnPropertyDescriptor(o, k); }

// seal: dense array
{
    let a = [1, 2, 3, 4, 5];
    Object.seal(a);
    shouldBe(Object.isSealed(a), true, "sealed");
    shouldBe(Object.isFrozen(a), false, "not frozen");
    shouldBe(desc(a, 0).configurable, false, "elem !configurable");
    shouldBe(desc(a, 0).writable, true, "elem writable");
    shouldBe(desc(a, 0).enumerable, true, "elem enumerable");
    shouldBe(desc(a, "length").writable, true, "length writable after seal");
    a[2] = 99;
    shouldBe(a[2], 99, "write ok");
}

// seal after preventExtensions
{
    let a = [1, 2, 3];
    Object.preventExtensions(a);
    shouldBe(Object.isSealed(a), false, "preventExtensions is not sealed");
    Object.seal(a);
    shouldBe(Object.isSealed(a), true);
    shouldBe(desc(a, 1).configurable, false);
}

// seal with indexed accessor
{
    let a = [1, 2, 3];
    let v = 0;
    Object.defineProperty(a, 1, { get: () => v, set: x => v = x, configurable: true });
    Object.seal(a);
    shouldBe(desc(a, 1).configurable, false);
    shouldBe(typeof desc(a, 1).get, "function");
    a[1] = 7;
    shouldBe(v, 7, "setter still runs");
}

// freeze: dense array
{
    let a = [1, 2, 3, 4, 5];
    Object.freeze(a);
    shouldBe(Object.isFrozen(a), true, "frozen");
    shouldBe(desc(a, 0).writable, false, "elem !writable");
    shouldBe(desc(a, 0).configurable, false, "elem !configurable");
    shouldBe(desc(a, "length").writable, false, "length !writable");
}

// freeze: empty array freezes length
{
    let a = [];
    Object.freeze(a);
    shouldBe(Object.isFrozen(a), true);
    shouldBe(desc(a, "length").writable, false, "empty array length frozen");
}

// freeze with indexed accessor: ReadOnly is not set on accessors
{
    let a = [1, 2, 3];
    let v = 0;
    Object.defineProperty(a, 1, { get: () => v, set: x => v = x, configurable: true });
    Object.freeze(a);
    shouldBe(desc(a, 1).configurable, false);
    shouldBe("writable" in desc(a, 1), false, "accessor has no writable");
    a[1] = 7;
    shouldBe(v, 7, "setter still runs on frozen accessor");
    shouldBe(desc(a, 0).writable, false);
}

// freeze after seal
{
    let a = [1, 2, 3];
    Object.seal(a);
    Object.freeze(a);
    shouldBe(Object.isFrozen(a), true);
    shouldBe(desc(a, 0).writable, false);
}

// seal/freeze holey array keeps the hole
{
    let a = [1, , 3];
    Object.seal(a);
    shouldBe(1 in a, false, "hole survives seal");

    let b = [1, , 3];
    Object.freeze(b);
    shouldBe(1 in b, false, "hole survives freeze");
}

// JSFinalObject with indexed properties
{
    let o = { 0: "a", 1: "b", x: 1 };
    Object.seal(o);
    shouldBe(Object.isSealed(o), true);
    shouldBe(desc(o, 0).configurable, false);
    shouldBe(desc(o, 0).writable, true);
    shouldBe(desc(o, "x").configurable, false);
    o[0] = "A";
    shouldBe(o[0], "A");
}
{
    let o = { 0: "a", 1: "b", x: 1 };
    Object.freeze(o);
    shouldBe(Object.isFrozen(o), true);
    shouldBe(desc(o, 0).writable, false);
    shouldBe(desc(o, "x").writable, false);
}

// DerivedArrayType takes the generic loop (canFastSetIntegrityLevel only
// matches exact ArrayType). Pins the invariant: if the gate is ever widened
// to inherits<JSArray>(), this still has to produce the same descriptors.
{
    class A extends Array {}
    let a = A.of(1, 2, 3);
    Object.freeze(a);
    shouldBe(Object.isFrozen(a), true, "derived frozen");
    shouldBe(desc(a, 0).writable, false, "derived elem !writable");
    shouldBe(desc(a, 0).configurable, false, "derived elem !configurable");
    shouldBe(desc(a, "length").writable, false, "derived length frozen");
}
{
    class A extends Array {}
    let a = A.of(1, 2, 3);
    Object.seal(a);
    shouldBe(Object.isSealed(a), true, "derived sealed");
    shouldBe(desc(a, 0).configurable, false, "derived elem !configurable");
    shouldBe(desc(a, 0).writable, true, "derived elem writable");
    shouldBe(desc(a, "length").writable, true, "derived length writable after seal");
}

// Proxy still sees the generic loop
{
    let calls = [];
    let target = [1, 2, 3];
    let p = new Proxy(target, {
        preventExtensions(t) { calls.push("pe"); return Reflect.preventExtensions(t); },
        ownKeys(t) { calls.push("ok"); return Reflect.ownKeys(t); },
        defineProperty(t, k, d) { calls.push("d:" + String(k)); return Reflect.defineProperty(t, k, d); },
    });
    Object.seal(p);
    shouldBe(calls.indexOf("pe") >= 0, true, "preventExtensions trap");
    shouldBe(calls.indexOf("ok") >= 0, true, "ownKeys trap");
    shouldBe(calls.indexOf("d:0") >= 0, true, "defineProperty trap per element");
}

// strict-mode write/delete semantics
(function () {
    "use strict";
    let a = Object.seal([1, 2, 3]);
    shouldThrow(() => { delete a[0]; }, "delete on sealed elem throws");
    shouldThrow(() => { a[3] = 4; }, "add on sealed array throws");
    shouldThrow(() => { a.length = 1; }, "shrink past non-configurable elem throws");

    let b = Object.freeze([1, 2, 3]);
    shouldThrow(() => { b[0] = 9; }, "write on frozen elem throws");
    shouldThrow(() => { b.length = 0; }, "length write on frozen throws");
})();

// Frozen object on the prototype chain: indexed writes through a child must
// respect the inherited ReadOnly entry. This exercises the
// notifyPresenceOfIndexedAccessors() call in the fast JSObject::freeze path.
(function () {
    "use strict";
    // Array prototype frozen before becoming a prototype.
    let p = Object.freeze([1, 2, 3]);
    let o = Object.create(p);
    shouldThrow(() => { o[0] = 99; }, "write to inherited frozen index throws (array proto)");
    shouldBe(Object.getOwnPropertyDescriptor(o, 0), undefined, "no own 0 after rejected write");
    shouldBe(o[0], 1, "inherited value intact");
    // Index past the frozen length is not inherited, so it defines an own property.
    o[5] = 5;
    shouldBe(o[5], 5);

    // Plain object prototype with indexed properties.
    let pp = Object.freeze({ 0: "a", 1: "b" });
    let oo = Object.create(pp);
    shouldThrow(() => { oo[0] = "x"; }, "write to inherited frozen index throws (object proto)");
    shouldBe(oo[0], "a");

    // Object already used as a prototype at freeze time.
    let q = [4, 5, 6];
    let child = Object.create(q);
    Object.freeze(q);
    shouldThrow(() => { child[1] = 0; }, "write to inherited frozen index throws (proto frozen after)");
    shouldBe(child[1], 5);

    // Seal does not affect writability, so the child write shadows the proto.
    let s = Object.seal([7, 8, 9]);
    let c = Object.create(s);
    c[0] = 77;
    shouldBe(c[0], 77, "write through sealed proto shadows");
    shouldBe(s[0], 7);
})();

// repeated application is idempotent
{
    let a = [1, 2, 3];
    Object.seal(a);
    Object.seal(a);
    Object.freeze(a);
    Object.freeze(a);
    shouldBe(Object.isFrozen(a), true);
}
