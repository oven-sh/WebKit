//@ runDefault("--compileMainScriptAheadOfTime=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function remarksOf(f) {
    let remarks = aotRemarks(f.name);
    if (!remarks && isAOTCompiled(f))
        throw new Error("no remarks for " + f.name);
    return remarks;
}
function matches(remark, pattern) { return remark === pattern || remark.startsWith(pattern + ":"); }
function applies(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (!remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " does not apply to " + f.name + ": " + remarks.join(" "));
    }
}
function doesNotApply(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " applies to " + f.name + ": " + remarks.join(" "));
    }
}
function repeat(f) {
    for (let i = 0; i < 100; i++)
        f(i);
}
function nothing() { }
noInline(nothing);

function readsTwice(o) { return o.a + o.a; }
function readsTwiceAndStores(o, more) { o.a = o.a + o.a + more; }
function testsFlags(o) { return o.flags & 1 && o.more & 2 ? "both" : o.flags & 4 ? "four" : "neither"; }
function comparesKind(o) { return o.kind === 1 || o.kind === 2 || o.kind === 3; }
function readsThroughParent(o) { return o.parent.kind === 1 && o.parent.name === "n"; }
function readsAfterStore(o, v) { o.a = v; return o.a; }
function readsAfterStoreToAnotherName(o, v) { const before = o.a; o.b = v; return before + o.a; }
function readsOnBothPaths(o, c) { const before = o.a; let extra = 0; if (c) extra = 1; else extra = 2; return before + o.a + extra; }
function readsAfterCall(o) { const before = o.a; nothing(); return before + o.a; }
function readsAfterStoreToSameName(o, p, v) { const before = o.a; p.a = v; return before + o.a; }
function readsAfterComputedStore(o, key, v) { const before = o.a; o[key] = v; return before + o.a; }
function readsAfterDelete(o) { const before = o.a; delete o.a; return String(before) + String(o.a); }
function readsOfTwoObjects(o, p) { return o.a + p.a; }
function readsTwoNames(o) { return o.a + o.b; }
function readsInLoop(o, n) { let sum = 0; for (let i = 0; i < n; i++) { sum += o.a; nothing(); } return sum; }
function readsAfterCallOnOnePath(o, c) { const before = o.a; if (c) nothing(); return before + o.a; }
function readsAfterConstruct(o) { const before = o.a; new nothing; return before + o.a; }
const all = [readsTwice, readsTwiceAndStores, testsFlags, comparesKind, readsThroughParent, readsAfterStore, readsAfterStoreToAnotherName, readsOnBothPaths, readsAfterCall,
    readsAfterStoreToSameName, readsAfterComputedStore, readsAfterDelete, readsOfTwoObjects, readsTwoNames, readsInLoop, readsAfterCallOnOnePath, readsAfterConstruct];
for (const f of all)
    noInline(f);

applies(readsTwice, "reuses-property-read:a");
applies(readsTwiceAndStores, "reuses-property-read:a");
applies(testsFlags, "reuses-property-read:flags");
applies(comparesKind, "reuses-property-read:kind");
applies(readsThroughParent, "reuses-property-read:parent");
applies(readsAfterStore, "reuses-property-read:a");
applies(readsAfterStoreToAnotherName, "reuses-property-read:a");
applies(readsOnBothPaths, "reuses-property-read:a");
for (const f of [readsAfterCall, readsAfterStoreToSameName, readsAfterComputedStore, readsAfterDelete, readsOfTwoObjects, readsTwoNames, readsInLoop, readsAfterCallOnOnePath, readsAfterConstruct])
    doesNotApply(f, "reuses-property-read");
doesNotApply(testsFlags, "reuses-property-read:more");

repeat(i => {
    check(readsTwice({ a: i }), 2 * i, "two reads");
    const stored = { a: i };
    readsTwiceAndStores(stored, 1);
    check(stored.a, 2 * i + 1, "two reads and a store");
    check(testsFlags({ flags: i & 7, more: i & 3 }), (i & 1) && (i & 2) ? "both" : i & 4 ? "four" : "neither", "flags");
    check(comparesKind({ kind: i % 5 }), i % 5 >= 1 && i % 5 <= 3, "kinds");
    check(readsThroughParent({ parent: { kind: i % 2, name: "n" } }), i % 2 === 1, "through a parent");
    check(readsAfterStore({ a: 0 }, i), i, "a read after a store");
    check(readsAfterStore({}, i), i, "a read after a store that adds the property");
    check(readsAfterStoreToAnotherName({ a: i, b: 0 }, 5), 2 * i, "a read after a store to another name");
    check(readsOnBothPaths({ a: i }, i & 1), 2 * i + (i & 1 ? 1 : 2), "a read after two paths meet");
    check(readsOfTwoObjects({ a: i }, { a: 1 }), i + 1, "two objects");
    check(readsTwoNames({ a: i, b: 1 }), i + 1, "two names");
});

repeat(i => {
    const same = { a: i };
    check(readsAfterStoreToSameName(same, same, 5), i + 5, "a store through another name for the same object");
    check(readsAfterStoreToSameName({ a: i }, { a: 0 }, 5), 2 * i, "a store to the same name of another object");
    const prototype = { a: i };
    check(readsAfterStoreToSameName(Object.create(prototype), prototype, 5), i + 5, "a store to the prototype that has the property");
    check(readsAfterComputedStore({ a: i }, "a", 5), i + 5, "a store under a computed name that is the same");
    check(readsAfterComputedStore({ a: i }, "b", 5), 2 * i, "a store under another computed name");
    check(readsAfterDelete({ a: i }), i + "undefined", "a read after the property is deleted");
});

{
    let calls = 0;
    const withGetter = { get a() { return ++calls; } };
    repeat(i => check(readsTwice(withGetter), 4 * i + 3, "a getter is called twice"));
    check(calls, 200, "calls of the getter");

    calls = 0;
    const inheriting = Object.create(withGetter);
    repeat(i => check(readsTwice(inheriting), 4 * i + 3, "an inherited getter is called twice"));
    check(calls, 200, "calls of the inherited getter");

    let gets = 0;
    const proxy = new Proxy({ a: 1 }, { get(target, key) { gets++; return target[key]; } });
    repeat(() => check(readsTwice(proxy), 2, "a proxy"));
    check(gets, 200, "calls of the trap");
}

{
    const changing = { flags: 1, get more() { this.flags = 4; return 0; } };
    repeat(() => {
        changing.flags = 1;
        check(testsFlags(changing), "four", "a getter of another property changes the one that was read");
    });

    const target = { a: 1 };
    const operand = { valueOf() { target.a = 10; return 100; } };
    function addsBetween(o, x) { return o.a + x + o.a; }
    noInline(addsBetween);
    applies(addsBetween, "reuses-property-read:a");
    repeat(() => {
        target.a = 1;
        check(addsBetween(target, operand), 111, "valueOf changes the property between two reads");
        target.a = 1;
        check(addsBetween(target, 100), 102, "a number between two reads");
    });

    function comparesBetween(o, x) { const before = o.a; return x < 5 ? before + o.a : -(before + o.a); }
    noInline(comparesBetween);
    applies(comparesBetween, "reuses-property-read:a");
    repeat(() => {
        target.a = 1;
        check(comparesBetween(target, operand), -11, "valueOf in a comparison changes the property");
        target.a = 1;
        check(comparesBetween(target, 1), 2, "a number in a comparison");
    });

    function comparesLooselyBetween(o, x) { const before = o.a; return x == 100 ? before + o.a : 0; }
    noInline(comparesLooselyBetween);
    repeat(() => {
        target.a = 1;
        check(comparesLooselyBetween(target, operand), 11, "valueOf in a loose comparison changes the property");
    });
}

{
    let stores = 0;
    const withSetter = { set a(v) { stores++; }, get a() { return "from the getter"; } };
    repeat(() => check(readsAfterStore(withSetter, 1), "from the getter", "a setter does not keep the value"));
    check(stores, 100, "calls of the setter");

    const changingSetter = { a: 1, set b(v) { this.a = v; } };
    repeat(i => {
        changingSetter.a = 1;
        check(readsAfterStoreToAnotherName(changingSetter, i), 1 + i, "a setter of another property changes the one that was read");
    });

    const frozen = Object.freeze({ a: "frozen" });
    repeat(() => check(readsAfterStore(frozen, 1), "frozen", "a store to a frozen object is ignored"));
    const readOnly = Object.defineProperty({}, "a", { value: "read-only", writable: false });
    repeat(() => check(readsAfterStore(readOnly, 1), "read-only", "a store to a read-only property is ignored"));
    const inheritsReadOnly = Object.create(readOnly);
    repeat(() => check(readsAfterStore(inheritsReadOnly, 1), "read-only", "a store that an inherited read-only property prevents"));
    const notExtensible = Object.preventExtensions({});
    repeat(() => check(readsAfterStore(notExtensible, 1), undefined, "a store to an object that cannot be extended"));
    repeat(() => check(readsAfterStore(5, 1), undefined, "a store to a number"));
    repeat(() => check(readsAfterStore("text", 1), undefined, "a store to a string"));

    let sets = 0;
    const proxy = new Proxy({}, { set() { sets++; return true; }, get() { return "from the trap"; } });
    repeat(() => check(readsAfterStore(proxy, 1), "from the trap", "a store to a proxy"));
    check(sets, 100, "calls of the set trap");

    function storesLength(array, n) { array.length = n; return array.length; }
    noInline(storesLength);
    repeat(() => check(storesLength([1, 2, 3], 1), 1, "the length of an array"));
}

{
    function accessProp(key) { return this[key]; }
    const mod = { b: 1 };
    const wrapper = {};
    Object.defineProperty(wrapper, "a", { get: accessProp.bind(mod, "b"), enumerable: true });
    function readsWrapper(w, m, v) { const before = w.a; m.b = v; return before + w.a; }
    noInline(readsWrapper);
    repeat(i => {
        mod.b = 1;
        check(readsWrapper(wrapper, mod, i), 1 + i, "a getter that reads another name of another object");
    });
}

{
    const counted = { a: 1 };
    let calls = 0;
    function changes() { calls++; counted.a++; }
    noInline(changes);
    function readsAroundCall(o) { const before = o.a; changes(); return before * 100 + o.a; }
    noInline(readsAroundCall);
    doesNotApply(readsAroundCall, "reuses-property-read");
    repeat(() => {
        counted.a = 1;
        check(readsAroundCall(counted), 102, "a call changes the property");
    });

    function readsAroundLoop(o, n) {
        const before = o.a;
        for (let i = 0; i < n; i++)
            changes();
        return before * 100 + o.a;
    }
    noInline(readsAroundLoop);
    repeat(() => {
        counted.a = 1;
        check(readsAroundLoop(counted, 2), 103, "a loop with a call changes the property");
        counted.a = 1;
        check(readsAroundLoop(counted, 0), 101, "a loop that does not run");
    });
    repeat(() => {
        counted.a = 1;
        check(readsAfterCallOnOnePath(counted, false), 2, "the path without the call");
    });
}

{
    function throwsBetween(o, x) {
        const before = o.a;
        try {
            x.missing.deeper;
        } catch {
            o.a = 7;
        }
        return before * 10 + o.a;
    }
    noInline(throwsBetween);
    repeat(() => {
        check(throwsBetween({ a: 1 }, {}), 17, "an exception handler changes the property");
        check(throwsBetween({ a: 1 }, { missing: {} }), 11, "no exception");
    });

    function* yieldsBetween(o) { const before = o.a; yield before; return before * 10 + o.a; }
    repeat(() => {
        const o = { a: 1 };
        const generator = yieldsBetween(o);
        check(generator.next().value, 1, "before the yield");
        o.a = 2;
        check(generator.next().value, 12, "the property changes while the generator is suspended");
    });

    async function awaitsBetween(o) { const before = o.a; await null; return before * 10 + o.a; }
    const o = { a: 1 };
    let result;
    awaitsBetween(o).then(value => { result = value; });
    o.a = 2;
    drainMicrotasks();
    check(result, 12, "the property changes while the function awaits");
}

{
    const shapes = [{ a: 1 }, { b: 0, a: 2 }, { c: 0, b: 0, a: 3 }, Object.create({ a: 4 }), { a: 5, z: 0 }, {}];
    const expected = [2, 4, 6, 8, 10, NaN];
    repeat(i => check(readsTwice(shapes[i % shapes.length]), expected[i % shapes.length], "several shapes at one site"));
    repeat(() => {
        let threw = false;
        try {
            readsTwice(undefined);
        } catch (error) {
            threw = error instanceof TypeError;
        }
        check(threw, true, "a read of undefined throws");
    });
}
