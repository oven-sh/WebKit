// Object.preventExtensions / seal / freeze and a read-only "length" keep the elements of an
// array in the ArrayStorage vector (SlowPutArrayStorage shape) instead of moving them into the
// sparse map. Every write path must still honour the attributes the structure records for them.

function shouldBe(actual, expected, msg) {
    if (actual !== expected)
        throw new Error((msg ? msg + ": " : "") + "expected " + String(expected) + " but got " + String(actual));
}
function shouldThrow(fn, errorType, msg) {
    let threw = null;
    try { fn(); } catch (e) { threw = e; }
    if (!threw)
        throw new Error((msg ? msg + ": " : "") + "expected throw");
    if (errorType && !(threw instanceof errorType))
        throw new Error((msg ? msg + ": " : "") + "expected " + errorType.name + " but got " + threw);
}
function desc(o, k) { return Object.getOwnPropertyDescriptor(o, k); }
function isSlowPut(a) { return $vm.indexingMode(a) === "ArrayWithSlowPutArrayStorage"; }
function ints(n) { return Array.from({ length: n }, (_, i) => i); }
function sum(a) { let s = 0; for (let i = 0; i < a.length; ++i) s += a[i]; return s; }
function sloppyWrite(o, i, v) { o[i] = v; }

// Representation after each producer.
{
    let a = ints(16);
    Object.preventExtensions(a);
    shouldBe(isSlowPut(a), true, "preventExtensions shape");
    shouldBe(sum(a), 120);

    let b = ints(16);
    Object.seal(b);
    shouldBe(isSlowPut(b), true, "seal shape");
    shouldBe(sum(b), 120);

    let c = ints(16);
    Object.freeze(c);
    shouldBe(isSlowPut(c), true, "freeze shape");
    shouldBe(sum(c), 120);

    let d = ints(16);
    Object.defineProperty(d, "length", { writable: false });
    shouldBe(isSlowPut(d), true, "length read-only shape");
    shouldBe(sum(d), 120);

    let e = [0.5, 1.5, 2.5];
    Object.freeze(e);
    shouldBe(isSlowPut(e), true, "double freeze shape");
    shouldBe(e[0] + e[1] + e[2], 4.5);

    let f = ["a", {}, 1];
    Object.freeze(f);
    shouldBe(isSlowPut(f), true, "contiguous freeze shape");
    shouldBe(f[0], "a");

    let g = [];
    Object.freeze(g);
    shouldBe(isSlowPut(g), true, "empty freeze shape");
    shouldBe(g.length, 0);
}

// preventExtensions: existing elements stay writable and deletable; nothing can be added.
(function () {
    "use strict";
    let a = [1, , 3];
    Object.preventExtensions(a);
    a[0] = 10;
    shouldBe(a[0], 10);
    shouldBe(Object.isExtensible(a), false);
    shouldBe(Object.isSealed(a), false);
    shouldBe(Object.isFrozen(a), false);
    shouldThrow(() => { a[1] = 2; }, TypeError, "fill hole");
    shouldBe(1 in a, false);
    shouldThrow(() => { a[3] = 4; }, TypeError, "append");
    shouldThrow(() => { a.push(4); }, TypeError, "push");
    shouldThrow(() => { a.unshift(0); }, TypeError, "unshift");
    shouldBe(a.length, 3);
    shouldBe(a.pop(), 3);
    shouldBe(a.length, 2);
    shouldBe(delete a[0], true);
    shouldBe(0 in a, false);
    a.length = 0;
    shouldBe(a.length, 0);
    // Only the non-configurable "length" is left.
    shouldBe(Object.isSealed(a), true);
    shouldBe(Object.isFrozen(a), false);
})();

// preventExtensions in sloppy mode: rejected writes are silent.
(function () {
    let a = [1, , 3];
    Object.preventExtensions(a);
    a[1] = 2;
    a[5] = 5;
    shouldBe(1 in a, false);
    shouldBe(a.length, 3);
})();

// seal: writable, not deletable, length can grow but not shrink over elements.
(function () {
    "use strict";
    let a = [1, 2, 3];
    Object.seal(a);
    a[0] = 10;
    shouldBe(a[0], 10);
    shouldThrow(() => { delete a[0]; }, TypeError, "delete");
    shouldThrow(() => { a.pop(); }, TypeError, "pop");
    shouldBe(a.join(), "10,2,3", "pop leaves the array alone");
    shouldThrow(() => { a.length = 1; }, TypeError, "shrink");
    shouldBe(a.length, 3, "length after rejected shrink");
    shouldThrow(() => { a.push(4); }, TypeError, "push");
    shouldBe(a.join(), "10,2,3");
    // shift and splice move the elements before the delete that fails, as in the spec.
    let s1 = Object.seal([10, 2, 3]);
    shouldThrow(() => { s1.shift(); }, TypeError, "shift");
    shouldBe(s1.join(), "2,3,3");
    let s2 = Object.seal([10, 2, 3]);
    shouldThrow(() => { s2.splice(0, 1); }, TypeError, "splice");
    shouldBe(s2.join(), "2,3,3");
    a.length = 10;
    shouldBe(a.length, 10);
    shouldThrow(() => { a[5] = 5; }, TypeError, "fill after grow");
    a.length = 3;
    shouldBe(a.length, 3);
    a.sort((x, y) => x - y);
    shouldBe(a.join(), "2,3,10", "sort in place");
    a.reverse();
    shouldBe(a.join(), "10,3,2", "reverse in place");
    a.fill(7);
    shouldBe(a.join(), "7,7,7", "fill in place");
    shouldBe(desc(a, 0).writable, true);
    shouldBe(desc(a, 0).configurable, false);
    shouldBe(desc(a, 0).enumerable, true);
    shouldBe(Object.isSealed(a), true);
    shouldBe(Object.isFrozen(a), false);
})();

// seal: length shrink stops at the highest element, as the spec's ArraySetLength does.
(function () {
    "use strict";
    let a = [1, 2, 3];
    a.length = 6;
    Object.seal(a);
    a.length = 4;
    shouldBe(a.length, 4, "shrink over holes");
    shouldThrow(() => { a.length = 0; }, TypeError);
    shouldBe(a.length, 3, "length stops after the last element");
})();

// freeze: nothing changes.
(function () {
    "use strict";
    let a = [1, 2, 3];
    Object.freeze(a);
    shouldThrow(() => { a[0] = 10; }, TypeError, "write");
    shouldThrow(() => { a[3] = 4; }, TypeError, "append");
    shouldThrow(() => { a.length = 0; }, TypeError, "shrink");
    shouldThrow(() => { a.length = 10; }, TypeError, "grow");
    shouldThrow(() => { a.push(4); }, TypeError, "push");
    shouldThrow(() => { a.pop(); }, TypeError, "pop");
    shouldThrow(() => { a.shift(); }, TypeError, "shift");
    shouldThrow(() => { a.unshift(0); }, TypeError, "unshift");
    shouldThrow(() => { a.splice(0, 1); }, TypeError, "splice");
    shouldThrow(() => { a.reverse(); }, TypeError, "reverse");
    shouldThrow(() => { a.sort(); }, TypeError, "sort");
    shouldThrow(() => { a.fill(0); }, TypeError, "fill");
    shouldThrow(() => { a.copyWithin(0, 1); }, TypeError, "copyWithin");
    shouldThrow(() => { delete a[0]; }, TypeError, "delete");
    shouldBe(a.join(), "1,2,3", "untouched");
    shouldBe(a.length, 3);
    shouldBe(desc(a, 0).writable, false);
    shouldBe(desc(a, 0).configurable, false);
    shouldBe(desc(a, 0).enumerable, true);
    shouldBe(desc(a, "length").writable, false);
    shouldBe(Object.isFrozen(a), true);
    shouldBe(Object.isSealed(a), true);
    shouldBe(Object.isExtensible(a), false);

    // Reads of every kind still work.
    shouldBe(a.indexOf(2), 1);
    shouldBe(a.includes(3), true);
    shouldBe(a.slice(1).join(), "2,3");
    shouldBe(a.concat([4]).join(), "1,2,3,4");
    shouldBe([...a].join(), "1,2,3");
    shouldBe(a.map(x => x * 2).join(), "2,4,6");
    shouldBe(JSON.stringify(a), "[1,2,3]");
    shouldBe(Object.keys(a).join(), "0,1,2");
    shouldBe(Object.getOwnPropertyNames(a).join(), "0,1,2,length");
    shouldBe(a.toReversed().join(), "3,2,1");
    shouldBe(a.with(0, 9).join(), "9,2,3");
    let count = 0;
    for (const x of a) count += x;
    shouldBe(count, 6);
    for (const k in a) count += Number(k);
    shouldBe(count, 9);
    shouldBe(a.hasOwnProperty(2), true);
    shouldBe(3 in a, false);
})();

// freeze in sloppy mode: rejected writes are silent, the generic algorithms still throw.
(function () {
    let a = [1, 2, 3];
    Object.freeze(a);
    a[0] = 10;
    a[3] = 4;
    a.length = 0;
    shouldBe(a.join(), "1,2,3");
    shouldThrow(() => { a.push(4); }, TypeError);
    shouldThrow(() => { a.pop(); }, TypeError);
})();

// Holes survive; a frozen hole reads through the prototype chain.
(function () {
    "use strict";
    let a = [1, , 3];
    Object.freeze(a);
    shouldBe(1 in a, false);
    shouldBe(a[1], undefined);
    shouldBe(a.length, 3);
    shouldThrow(() => { a[1] = 2; }, TypeError);
})();

// Read-only length alone: elements stay writable, the array cannot grow.
(function () {
    "use strict";
    let a = [1, 2, 3];
    a.push(4, 5, 6, 7, 8);
    a.length = 3; // Leaves spare vector capacity behind the length.
    Object.defineProperty(a, "length", { writable: false });
    a[0] = 10;
    shouldBe(a[0], 10);
    shouldThrow(() => { a.push(4); }, TypeError, "push");
    shouldThrow(() => { a[3] = 4; }, TypeError, "append");
    shouldThrow(() => { a.length = 0; }, TypeError, "shrink");
    shouldThrow(() => { a.length = 10; }, TypeError, "grow");
    shouldThrow(() => { a.pop(); }, TypeError, "pop");
    shouldBe(a.length, 3);
    shouldBe(delete a[2], true, "delete stays allowed");
    shouldBe(a.length, 3);
    shouldBe(Object.isExtensible(a), true);
    shouldBe(Object.isFrozen(a), false);
    shouldBe(desc(a, "length").writable, false);
})();

// Spare vector capacity behind the length does not let push or a hole write through.
(function () {
    "use strict";
    let a = [];
    for (let i = 0; i < 5; ++i) a.push(i);
    a.length = 2;
    Object.freeze(a);
    shouldThrow(() => { a.push(9); }, TypeError);
    shouldThrow(() => { a[2] = 9; }, TypeError);
    shouldThrow(() => { a[4] = 9; }, TypeError);
    shouldBe(a.length, 2);
    shouldBe(a.join(), "0,1");

    let b = [];
    for (let i = 0; i < 5; ++i) b.push(i);
    b.length = 2;
    Object.seal(b);
    shouldThrow(() => { b.push(9); }, TypeError);
    shouldThrow(() => { b[2] = 9; }, TypeError);
    shouldBe(b.length, 2);
})();

// Object.defineProperty on an element after seal / freeze migrates to the sparse map with the
// exact descriptors.
(function () {
    "use strict";
    let a = [1, 2, 3];
    Object.seal(a);
    Object.defineProperty(a, 0, { writable: false });
    shouldBe(desc(a, 0).writable, false);
    shouldBe(desc(a, 0).configurable, false);
    shouldBe(desc(a, 1).writable, true);
    shouldBe(desc(a, 1).configurable, false);
    shouldThrow(() => { a[0] = 5; }, TypeError);
    a[1] = 5;
    shouldBe(a[1], 5);
    shouldThrow(() => Object.defineProperty(a, 1, { configurable: true }), TypeError);
    shouldThrow(() => Object.defineProperty(a, 1, { get() {} }), TypeError);
    shouldThrow(() => Object.defineProperty(a, 3, { value: 1 }), TypeError);
    Object.defineProperty(a, 1, { value: 6 });
    shouldBe(a[1], 6);
    shouldBe(Object.isSealed(a), true);
    shouldBe(Object.isFrozen(a), false);
    Object.defineProperty(a, 1, { writable: false });
    Object.defineProperty(a, 2, { writable: false });
    Object.defineProperty(a, "length", { writable: false });
    shouldBe(Object.isFrozen(a), true, "frozen through defineProperty");

    let b = [1, 2, 3];
    Object.freeze(b);
    Object.defineProperty(b, 0, { value: 1 });
    shouldThrow(() => Object.defineProperty(b, 0, { value: 2 }), TypeError);
    shouldThrow(() => Object.defineProperty(b, 0, { writable: true }), TypeError);
    shouldThrow(() => Object.defineProperty(b, 0, { enumerable: false }), TypeError);
    shouldBe(Reflect.defineProperty(b, 0, { value: 2 }), false);
    shouldBe(b.join(), "1,2,3");
    shouldBe(Object.isFrozen(b), true);
})();

// Seal then freeze, freeze twice, preventExtensions then seal then freeze.
(function () {
    "use strict";
    let a = [1, 2, 3];
    Object.preventExtensions(a);
    Object.seal(a);
    shouldBe(Object.isSealed(a), true);
    a[0] = 9;
    Object.freeze(a);
    shouldBe(Object.isFrozen(a), true);
    shouldThrow(() => { a[0] = 1; }, TypeError);
    Object.freeze(a);
    shouldBe(a.join(), "9,2,3");
})();

// An array that already owns a sparse map entry (an accessor) is sealed / frozen together with it.
(function () {
    "use strict";
    let a = [1, 2, 3];
    let v = 0;
    Object.defineProperty(a, 1, { get: () => v, set: x => { v = x; }, configurable: true });
    Object.freeze(a);
    shouldBe(desc(a, 1).configurable, false);
    shouldBe(desc(a, 0).writable, false);
    shouldBe(desc(a, 0).configurable, false);
    a[1] = 7;
    shouldBe(v, 7, "setter still runs");
    shouldThrow(() => { a[0] = 7; }, TypeError);
    shouldBe(Object.isFrozen(a), true);
})();

// A frozen array on the prototype chain: inherited elements are read-only for the child.
(function () {
    "use strict";
    let proto = Object.freeze([1, 2, 3]);
    function write(o, i, v) { o[i] = v; }
    noInline(write);
    for (let i = 0; i < 1e4; ++i) {
        let o = Object.create(proto);
        shouldThrow(() => write(o, 0, 99), TypeError, "inherited write " + i);
        shouldBe(o[0], 1);
        shouldBe(o.hasOwnProperty(0), false);
        write(o, 5, 5);
        shouldBe(o[5], 5);
    }
    let sloppyChild = Object.create(proto);
    sloppyWrite(sloppyChild, 0, 99);
    shouldBe(sloppyChild[0], 1);
    shouldBe(sloppyChild.hasOwnProperty(0), false);

    // Object.setPrototypeOf on a child that already has contiguous storage.
    let grown = [0, , 0];
    grown[0] = 1;
    Object.setPrototypeOf(grown, proto);
    shouldThrow(() => { grown[1] = 9; }, TypeError, "inherited write after setPrototypeOf");
    shouldBe(grown.hasOwnProperty(1), false);
    shouldBe(grown[1], 2);
    grown[0] = 5;
    shouldBe(grown[0], 5);
    grown[3] = 3;
    shouldBe(grown.length, 4);

    // A frozen array that is no prototype keeps the builtin fast paths of a plain array.
    shouldBe(proto.slice(1).join(), "2,3");
    shouldBe(proto.indexOf(3), 2);
    shouldBe([...proto].join(), "1,2,3");

    // A sealed prototype does not stop the child from shadowing.
    let sealed = Object.seal([7, 8, 9]);
    let c = Object.create(sealed);
    c[0] = 77;
    shouldBe(c[0], 77);
    shouldBe(sealed[0], 7);
})();

// A store site that inlined the SlowPutArrayStorage store (warmed on a non-extensible but
// writable array) must still reject the write to a frozen array in every tier.
(function () {
    function store(a, i, v) { "use strict"; a[i] = v; }
    noInline(store);
    let writable = ints(8);
    Object.preventExtensions(writable);
    for (let i = 0; i < 1e5; ++i)
        store(writable, i & 7, i);
    shouldBe(writable[7], 99999);
    let frozen = Object.freeze(ints(8));
    for (let i = 0; i < 1e3; ++i) {
        shouldThrow(() => store(frozen, i & 7, -1), TypeError, "frozen store " + i);
        store(writable, i & 7, -1);
    }
    shouldBe(frozen.join(), "0,1,2,3,4,5,6,7");
    shouldBe(writable.join(), "-1,-1,-1,-1,-1,-1,-1,-1");
    let sealed = Object.seal(ints(8));
    for (let i = 0; i < 1e3; ++i)
        store(sealed, i & 7, i);
    shouldBe(sealed[7], 999);

    function storeSloppy(a, i, v) { a[i] = v; }
    noInline(storeSloppy);
    for (let i = 0; i < 1e5; ++i)
        storeSloppy(writable, i & 7, i);
    for (let i = 0; i < 1e3; ++i)
        storeSloppy(frozen, i & 7, -1);
    shouldBe(frozen.join(), "0,1,2,3,4,5,6,7");

    // Out-of-bounds stores on the same warmed site.
    function storeOOB(a, i, v) { "use strict"; a[i] = v; }
    noInline(storeOOB);
    let growing = [];
    for (let i = 0; i < 1e5; ++i) {
        if (growing.length > 64) growing = [];
        storeOOB(growing, growing.length, i);
    }
    let writableOOB = ints(4);
    Object.preventExtensions(writableOOB);
    for (let i = 0; i < 1e4; ++i)
        storeOOB(writableOOB, i & 3, i);
    shouldThrow(() => storeOOB(writableOOB, 4, 4), TypeError);
    shouldThrow(() => storeOOB(frozen, 8, 8), TypeError);
    shouldThrow(() => storeOOB(frozen, 0, 8), TypeError);
    shouldBe(frozen.length, 8);
})();

// Reads in every tier see the vector.
(function () {
    function read(a, i) { return a[i]; }
    noInline(read);
    let frozen = Object.freeze(ints(16));
    let sealed = Object.seal(ints(16));
    let plain = ints(16);
    let total = 0;
    for (let i = 0; i < 1e5; ++i)
        total += read(frozen, i & 15) + read(sealed, i & 15) + read(plain, i & 15);
    shouldBe(total, 3 * 100000 * 7.5);
    shouldBe(read(frozen, 16), undefined);
    shouldBe(read(frozen, -1), undefined);
    shouldBe(read(frozen, "length"), 16);
})();

// A frozen element of a constant array folds in the DFG; the value must be right.
const frozenTable = Object.freeze([10, 20, 30, 40]);
(function () {
    function lookup() { return frozenTable[2]; }
    noInline(lookup);
    for (let i = 0; i < 1e5; ++i)
        shouldBe(lookup(), 30);
})();

// Freezing one array from a literal site leaves the site and other literals alone.
(function () {
    "use strict";
    function make() { return [1, 2, 3, 4]; }
    noInline(make);
    for (let i = 0; i < 100; ++i) {
        let a = make();
        Object.freeze(a);
        shouldThrow(() => { a[0] = 9; }, TypeError);
        let b = make();
        b[0] = 9;
        b.push(5);
        shouldBe(b.join(), "9,2,3,4,5");
        if (!$vm.isHavingABadTime(b))
            shouldBe(isSlowPut(b), false, "literal after a frozen sibling");
        shouldBe(a.join(), "1,2,3,4");
    }
    function build(n) { let a = []; for (let i = 0; i < n; ++i) a.push(i); return a; }
    noInline(build);
    for (let i = 0; i < 100; ++i) {
        Object.freeze(build(8));
        let b = build(8);
        if (!$vm.isHavingABadTime(b))
            shouldBe(isSlowPut(b), false, "built after a frozen sibling");
        b[0] = 1;
    }
})();

// Freezing an array that is already a prototype makes every array slow (haveABadTime), as
// defining a read-only index on it always did. Keep this after the tests that look at shapes.
(function () {
    "use strict";
    let late = [4, 5, 6];
    let child = Object.create(late);
    Object.freeze(late);
    shouldThrow(() => { child[1] = 0; }, TypeError);
    shouldBe(child[1], 5);
    shouldBe(late.join(), "4,5,6");
})();

// Having a bad time after arrays were frozen.
(function () {
    "use strict";
    let frozen = Object.freeze([1, 2, 3]);
    let sealed = Object.seal([1, 2, 3]);
    let plain = [1, 2, 3];
    let holey = Object.freeze([1, , 3]);
    Object.defineProperty(Array.prototype, 1, { get() { return "proto"; }, configurable: true });
    try {
        shouldBe(holey[1], "proto", "a frozen hole reads through the prototype chain");
        shouldBe(holey.hasOwnProperty(1), false);
    } finally {
        delete Array.prototype[1];
    }
    Object.defineProperty(Object.prototype, 7, { set(v) { this.intercepted = v; }, configurable: true });
    try {
        shouldThrow(() => { frozen[0] = 9; }, TypeError);
        sealed[0] = 9;
        shouldBe(sealed[0], 9);
        plain[7] = 1;
        shouldBe(plain.intercepted, 1);
        shouldBe(plain.length, 3);
        shouldThrow(() => { frozen[7] = 1; }, TypeError);
        shouldBe(frozen.intercepted, undefined);
        shouldBe(frozen.join(), "1,2,3");
        shouldBe(Object.isFrozen(frozen), true);
        let late = Object.freeze([4, 5]);
        shouldThrow(() => { late[0] = 9; }, TypeError);
        shouldBe(late.join(), "4,5");
    } finally {
        delete Object.prototype[7];
    }
})();

// An array that owns a sparse map entry far beyond its vector goes fully sparse: a write to a
// hole between the vector and that entry cannot grow the vector of a non-extensible object.
(function () {
    "use strict";
    let a = [1, 2, 3];
    a[100000] = 4;
    Object.seal(a);
    shouldThrow(() => { a[50] = 5; }, TypeError, "hole write with an overflow map");
    shouldBe(50 in a, false);
    a[0] = 9;
    shouldBe(a[0], 9);
    shouldBe(a[100000], 4);
    shouldThrow(() => { delete a[100000]; }, TypeError);
    shouldBe(a.length, 100001);
    let b = [1, 2, 3];
    b[100000] = 4;
    Object.freeze(b);
    shouldThrow(() => { b[0] = 9; }, TypeError);
    shouldThrow(() => { b[100000] = 9; }, TypeError);
    shouldBe(Object.isFrozen(b), true);
})();

// Plain objects with indexed properties.
(function () {
    "use strict";
    let o = { 0: "a", 1: "b", x: 1 };
    Object.freeze(o);
    shouldThrow(() => { o[0] = "z"; }, TypeError);
    shouldThrow(() => { o[2] = "c"; }, TypeError);
    shouldThrow(() => { delete o[0]; }, TypeError);
    shouldBe(o[0], "a");
    shouldBe(Object.isFrozen(o), true);
    let p = { 0: "a", 1: "b" };
    Object.preventExtensions(p);
    p[0] = "z";
    shouldBe(p[0], "z");
    shouldThrow(() => { p[2] = "c"; }, TypeError);
    shouldBe(delete p[1], true);
})();

// Class extends Array takes the generic path and ends up frozen all the same.
(function () {
    "use strict";
    class A extends Array {}
    let a = A.of(1, 2, 3);
    Object.freeze(a);
    shouldBe(Object.isFrozen(a), true);
    shouldThrow(() => { a[0] = 9; }, TypeError);
    shouldThrow(() => { a.push(4); }, TypeError);
    shouldBe(a.join(), "1,2,3");
})();
