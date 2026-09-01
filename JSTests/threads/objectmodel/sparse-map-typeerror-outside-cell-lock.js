//@ requireOptions("--useJSThreads=1", "--gcMaxHeapSize=200000")
// Flag-on, SparseArrayValueMap::putEntry/putDirect decide the outcome of a
// write under the map's cell lock. The TypeError for a rejected write (and
// the setter of an accessor entry) must run after the lock drops: creating
// the error allocates, a collection triggered by that allocation visits the
// map under the same lock, and the mutator self-deadlocks. --gcMaxHeapSize
// makes a collection certain within the first few thousand rejected writes.
// Single-threaded and deterministic; every case also checks the observable
// semantics of the rewritten paths (rejects leave no placeholder entry,
// accessors are dispatched on the value, length shrink honours DontDelete).
"use strict";
load("../harness.js", "caller relative");

const BASE = 1e6; // far beyond any dense vector: every index below is a sparse-map entry

function expectTypeError(f, what) {
    try {
        f();
    } catch (e) {
        if (!(e instanceof TypeError))
            throw new Error(what + ": threw " + e + " instead of a TypeError");
        return;
    }
    throw new Error(what + ": did not throw");
}

// Rejected strict writes to a read-only sparse entry, in a loop that
// allocates only through the rejection path.
{
    const a = [];
    a[BASE] = 1;
    Object.defineProperty(a, BASE + 5, { value: 42, writable: false, configurable: true, enumerable: true });
    let caught = 0;
    for (let i = 0; i < 4000; ++i) {
        try {
            a[BASE + 5] = i;
        } catch (e) {
            if (!(e instanceof TypeError))
                throw new Error("read-only sparse write threw " + e);
            ++caught;
        }
    }
    if (caught !== 4000)
        throw new Error("read-only sparse write: " + caught + " of 4000 writes rejected");
    if (a[BASE + 5] !== 42)
        throw new Error("read-only sparse entry changed to " + a[BASE + 5]);
    if (Reflect.set(a, BASE + 5, 7) !== false)
        throw new Error("Reflect.set on a read-only sparse entry returned true");
    if (a[BASE + 5] !== 42)
        throw new Error("read-only sparse entry changed by Reflect.set");
    a[BASE] = 2;
    if (a[BASE] !== 2)
        throw new Error("writable sparse entry not updated");
}

// Accessor entries: dispatched on the stored GetterSetter, setter runs with
// the lock dropped, a missing setter rejects.
{
    const a = [];
    a[BASE] = 1;
    let received;
    Object.defineProperty(a, BASE + 3, { get() { return "g"; }, set(v) { received = v; }, configurable: true });
    for (let i = 0; i < 100; ++i)
        a[BASE + 3] = i;
    if (received !== 99)
        throw new Error("sparse setter saw " + received);
    if (a[BASE + 3] !== "g")
        throw new Error("sparse getter returned " + a[BASE + 3]);
    Object.defineProperty(a, BASE + 4, { get() { return 1; }, configurable: true });
    for (let i = 0; i < 1000; ++i)
        expectTypeError(() => { a[BASE + 4] = i; }, "write to a getter-only sparse entry");
}

// Non-extensible arrays: a rejected write publishes no entry.
{
    const a = [];
    a[BASE] = 1;
    Object.preventExtensions(a);
    for (let i = 0; i < 1000; ++i)
        expectTypeError(() => { a[BASE + 7] = i; }, "write to a non-extensible sparse array");
    if ((BASE + 7) in a)
        throw new Error("rejected write left an entry behind");
    if (Reflect.set(a, BASE + 9, 1) !== false || (BASE + 9) in a)
        throw new Error("Reflect.set on a non-extensible sparse array added an entry");
    if (Reflect.defineProperty(a, BASE + 11, { value: 1 }) !== false || (BASE + 11) in a)
        throw new Error("defineProperty on a non-extensible sparse array added an entry");
    if (Object.getOwnPropertyNames(a).length !== 2) // "1000000" and "length"
        throw new Error("unexpected own properties: " + Object.getOwnPropertyNames(a));
    a[BASE] = 2;
    if (a[BASE] !== 2)
        throw new Error("existing entry of a non-extensible array not updated");

    const frozen = [];
    frozen[BASE] = 1;
    Object.freeze(frozen);
    for (let i = 0; i < 1000; ++i) {
        expectTypeError(() => { frozen[BASE] = i; }, "write to a frozen sparse entry");
        expectTypeError(() => { frozen[BASE + 1] = i; }, "add to a frozen sparse array");
    }
    if (!Object.isFrozen(frozen) || frozen[BASE] !== 1 || (BASE + 1) in frozen)
        throw new Error("frozen sparse array changed");
}

// defineProperty reconfiguration of sparse entries (data <-> accessor,
// attribute-only), including moving dense values into the map.
{
    const a = [1, 2, 3];
    Object.defineProperty(a, 1, { get() { return "acc"; }, configurable: true });
    if (a[0] !== 1 || a[1] !== "acc" || a[2] !== 3)
        throw new Error("dense values lost entering dictionary indexing: " + a);
    Object.defineProperty(a, 1, { value: "data" });
    let d = Object.getOwnPropertyDescriptor(a, 1);
    if (d.value !== "data" || d.writable !== false || d.enumerable !== true || d.configurable !== true)
        throw new Error("accessor -> data reconfiguration: " + JSON.stringify(d));
    Object.defineProperty(a, 1, { enumerable: false });
    d = Object.getOwnPropertyDescriptor(a, 1);
    if (d.value !== "data" || d.writable !== false || d.enumerable !== false)
        throw new Error("attribute-only reconfiguration: " + JSON.stringify(d));
    Object.defineProperty(a, 1, { set(v) { this.last = v; } });
    a[1] = 5;
    d = Object.getOwnPropertyDescriptor(a, 1);
    if (a.last !== 5 || d.value !== undefined || typeof d.set !== "function" || d.get !== undefined)
        throw new Error("data -> accessor reconfiguration: " + JSON.stringify(d));
    Object.defineProperty(a, 1, { value: 9, writable: true, enumerable: true, configurable: false });
    expectTypeError(() => Object.defineProperty(a, 1, { configurable: true }), "making a non-configurable entry configurable");
    expectTypeError(() => Object.defineProperty(a, 1, { get() { } }), "turning a non-configurable data entry into an accessor");
    a[1] = 10;
    if (a[1] !== 10)
        throw new Error("writable non-configurable entry not updated");
}

// Length shrink over sparse entries: DontDelete stops the shrink at its
// index; entries above it are gone.
{
    const a = [];
    a[BASE] = 1;
    a[BASE + 1] = 2;
    Object.defineProperty(a, BASE + 2, { value: 3, writable: true, enumerable: true, configurable: false });
    a[BASE + 3] = 4;
    a[BASE + 4] = 5;
    expectTypeError(() => { a.length = 10; }, "shrinking over a non-configurable sparse entry");
    if (a.length !== BASE + 3)
        throw new Error("length after the rejected shrink: " + a.length);
    if ((BASE + 3) in a || (BASE + 4) in a)
        throw new Error("entries above the non-configurable entry survived");
    if (a[BASE] !== 1 || a[BASE + 1] !== 2 || a[BASE + 2] !== 3)
        throw new Error("entries at or below the non-configurable entry changed");

    const b = [];
    b[BASE] = 1;
    b[BASE + 1] = 2;
    b[BASE + 2] = 3;
    b.length = BASE + 1;
    if (b.length !== BASE + 1 || b[BASE] !== 1 || (BASE + 1) in b || (BASE + 2) in b)
        throw new Error("plain shrink of a sparse array: " + b.length);
    b.length = 0;
    if (b.length !== 0 || BASE in b)
        throw new Error("shrink to zero of a sparse array");
}
