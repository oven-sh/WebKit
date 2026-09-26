function assert(cond, msg) {
    if (!cond)
        throw new Error("FAIL: " + msg);
}

function warm(f, n = 1e4) {
    let r;
    for (let i = 0; i < n; i++)
        r = f(i);
    return r;
}

{
    const proto = { method() { return 1; } };
    const o = Object.create(proto);
    const read = () => o.method();
    warm(read);
    Object.defineProperty(proto, "method", { writable: false });
    assert(warm(read) === 1, "same-value attribute change keeps value");
    Object.defineProperty(proto, "method", { value: () => 2 });
    assert(warm(read) === 2, "configurable read-only property redefined with a new value is observed");
}

{
    const origExec = RegExp.prototype.exec;
    const run = () => "a-b".replace(/-/g, "+");
    warm(run, 1e3);
    Object.defineProperty(RegExp.prototype, "exec", { writable: false });
    assert(run() === "a+b", "replace still works after exec made read-only");
    let called = 0;
    Object.defineProperty(RegExp.prototype, "exec", { value: function (s) { called++; return origExec.call(this, s); } });
    assert(run() === "a+b" && called > 0, "replaced exec is called by String.prototype.replace");
    Object.defineProperty(RegExp.prototype, "exec", { value: origExec });
}

{
    const o = {};
    const g1 = () => 1;
    const g2 = () => 2;
    Object.defineProperty(o, "x", { get: g1, configurable: true });
    const read = () => o.x;
    warm(read);
    Object.defineProperty(o, "x", { enumerable: true });
    assert(warm(read) === 1, "accessor attribute change keeps getter");
    const desc = Object.getOwnPropertyDescriptor(o, "x");
    assert(desc.get === g1 && desc.enumerable, "descriptor updated");
    Object.defineProperty(o, "x", { get: g2 });
    assert(warm(read) === 2, "new getter observed");
    Object.defineProperty(o, "x", { set(v) { this._v = v; } });
    assert(o.x === 2, "getter preserved when only setter changes");
    o.x = 5;
    assert(o._v === 5, "new setter called");
    Object.defineProperty(o, "x", { get: undefined });
    assert(o.x === undefined, "getter cleared");
}

{
    class MyArray extends Array { }
    const a = MyArray.from([1, 2, 3]);
    Object.defineProperty(Array, Symbol.species, { configurable: false });
    assert(a.map(x => x) instanceof MyArray, "subclass species still honored");
    assert([1, 2].map(x => x).constructor === Array, "plain array species");
}

{
    Object.freeze(Object.prototype);
    Object.freeze(Array.prototype);
    Object.freeze(Function.prototype);
    Object.freeze(RegExp.prototype);
    Object.freeze(String.prototype);
    Object.freeze(Promise.prototype);
    Object.freeze(Map.prototype);
    Object.freeze(Set.prototype);
    assert(Object.isFrozen(Object.prototype) && Object.isFrozen(RegExp.prototype), "isFrozen");
    for (let i = 0; i < 1e3; i++) {
        assert("a-b".replace(/-/g, "+") === "a+b", "replace after freeze");
        assert([..."abc"].join("") === "abc", "string spread after freeze");
        assert(String(new String("x")) === "x", "String(obj) after freeze");
        assert([1, 2] + "" === "1,2", "array join after freeze");
        assert([...new Set([1, 2])].length === 2 && new Map([[1, 2]]).get(1) === 2, "Map/Set after freeze");
    }
    let threw = false;
    try {
        (() => { "use strict"; ({}).toString = 1; })();
    } catch {
        threw = true;
    }
    assert(threw, "override mistake still throws");
    assert(Object.getOwnPropertyDescriptor(RegExp.prototype, "flags").configurable === false, "accessor frozen");
}
