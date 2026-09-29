// Whether a property can be set or deleted is for JavaScript to say, whichever language asks.
import m from "./setting-what-javascript-guards.py";
const s = v => JSON.stringify(v);
let seen = [];
const kinds = {
    "plain": () => ({ a: 1 }),
    "empty": () => ({}),
    "frozen": () => Object.freeze({ a: 1 }),
    "frozen and empty": () => Object.freeze({}),
    "sealed": () => Object.seal({ a: 1 }),
    "sealed and empty": () => Object.seal({}),
    "not extensible": () => Object.preventExtensions({ a: 1 }),
    "not extensible and empty": () => Object.preventExtensions({}),
    "not writable": () => Object.defineProperty({}, "a", { value: 1, writable: false, enumerable: true, configurable: true }),
    "not configurable": () => Object.defineProperty({}, "a", { value: 1, writable: true, enumerable: true, configurable: false }),
    "a getter alone": () => ({ get a() { return 1; } }),
    "a setter": () => ({ set a(v) { seen.push("set " + v); } }),
    "both": () => ({ get a() { return "got"; }, set a(v) { seen.push("set " + v); } }),
    "a setter that throws": () => ({ set a(v) { throw new RangeError("not " + v); } }),
    "a setter that is inherited": () => Object.create({ set a(v) { seen.push("set " + v + " on " + (Object.getPrototypeOf(this) !== null)); } }),
    "a getter alone that is inherited": () => Object.create({ get a() { return 1; } }),
    "what is inherited is not writable": () => Object.create(Object.freeze({ a: 1 })),
    "what is inherited is": () => Object.create({ a: 1 }),
    "a setter of a class": () => new (class Guarded { set a(v) { seen.push("set " + v); } })(),
    "no prototype": () => Object.create(null),
    "a proxy": () => new Proxy({}, {
        set(t, k, v) { seen.push("set " + String(k) + " " + v); t[k] = v; return true; },
        deleteProperty(t, k) { seen.push("delete " + String(k)); return delete t[k]; },
        getOwnPropertyDescriptor(t, k) { seen.push("describe " + String(k)); return Reflect.getOwnPropertyDescriptor(t, k); },
    }),
    "a proxy that has it": () => new Proxy({ a: 1 }, { deleteProperty(t, k) { seen.push("delete " + String(k)); return delete t[k]; } }),
    "a proxy that says no": () => new Proxy({ a: 1 }, { set() { return false; }, deleteProperty() { return false; } }),
    "a proxy that throws": () => new Proxy({ a: 1 }, { set() { throw new EvalError("set"); }, deleteProperty() { throw new EvalError("delete"); } }),
    // It could be a class, and what is set on a class is for its instances.
    "a function": () => function f() {},
    "a frozen function": () => Object.freeze(function f() {}),
    "an arrow function": () => () => {},
    "a frozen arrow function": () => Object.freeze(() => {}),
    "an array": () => [1],
    "a frozen array": () => Object.freeze([1]),
    "a map": () => new Map(),
    "a frozen map": () => Object.freeze(new Map()),
    "a promise": () => Promise.resolve(1),
    "a frozen promise": () => Object.freeze(Promise.resolve(1)),
    "a regular expression": () => /x/,
    "arguments": () => (function () { return arguments; })(),
    "a typed array": () => new Float64Array(1),
    "a date": () => new Date(0),
};
function state(o) {
    const d = Object.getOwnPropertyDescriptor(o, "a");
    return [d ? ("value" in d ? s(d.value) : "accessor") : "none", Object.isFrozen(o) ? "frozen" : Object.isSealed(o) ? "sealed" : Object.isExtensible(o) ? "" : "closed"].join(" ").trim();
}
for (const [name, make] of Object.entries(kinds)) {
    // Each way of saying it comes to the same, and comes to the same again once it has been compiled.
    for (const [what, ways] of [["set", m.setters], ["delete", m.deleters]]) {
        const results = new Set();
        for (const way of ways) {
            for (let i = 0; i < 40; ++i) {
                const o = make();
                const before = state(o);
                seen = [];
                const result = what === "set" ? way(o, 2) : way(o);
                const said = seen.slice();
                results.add(`${result} | ${before} -> ${state(o)}${said.length ? " | " + said.join(", ") : ""}`);
            }
        }
        print(name + ",", what, "=>", [...results].join("  BUT ALSO  "));
    }
}
print("what Python says of the like =>", s(m.frozen_dataclass()));
print("derived =>", s(m.derived().map(String)));
print("the global object =>", s(m.the_global_object()));
for (const name of ["plain", "frozen", "a getter alone", "a setter", "a proxy that has it", "a proxy that throws", "an arrow function", "an array", "a map", "no prototype", "a typed array"])
    print("__dict__ of", name, "=>", s(m.its_dict(kinds[name]())));
print("__dict__ of what has more =>", s(m.its_dict(Object.defineProperties({ a: 1, 0: "index", [Symbol("s")]: 2, get g() { return "got"; } }, { hidden: { value: 1, enumerable: false } }))));
print("__dict__ when a getter throws =>", s(m.its_dict({ get a() { throw new RangeError("no"); } })));

// A function of Python's is a function like any other to JavaScript, which can do to it what it can do to any. And an Error that JavaScript made is JavaScript's, though its class may be Python's.
const shared = {
    "a function of Python's": m.make_function,
    "a TypeError of JavaScript's": () => new TypeError("js"),
    "an Error of JavaScript's": () => new Error("js"),
};
const done = {
    "nothing": o => o,
    "a value": o => { o.a = 1; return o; },
    "a getter": o => Object.defineProperty(o, "a", { get() { return "got"; }, enumerable: true, configurable: true }),
    "a getter and a setter": o => Object.defineProperty(o, "a", { get() { return "got"; }, set(v) { seen.push("set " + v); }, enumerable: true, configurable: true }),
    "one that is hidden": o => Object.defineProperty(o, "a", { value: 1, enumerable: false, writable: true, configurable: true }),
    "one that is not writable": o => Object.defineProperty(o, "a", { value: 1, enumerable: true, writable: false, configurable: true }),
    "one that is not configurable": o => Object.defineProperty(o, "a", { value: 1, enumerable: true, writable: true, configurable: false }),
    "frozen": o => Object.freeze(o),
    "a value, and frozen": o => { o.a = 1; return Object.freeze(o); },
    "sealed": o => Object.seal(o),
    "a value, and sealed": o => { o.a = 1; return Object.seal(o); },
};
for (const [kind, make] of Object.entries(shared)) {
    for (const [what, prepare] of Object.entries(done)) {
        print(kind + ",", what + ":");
        print("    seen =>", s(m.seen(prepare(make()))));
        for (const [verb, ways] of [["set", [...m.setters, m.by_dict]], ["delete", [...m.deleters, m.delete_by_dict]]]) {
            const results = [];
            for (const way of ways) {
                const o = prepare(make());
                const before = state(o);
                seen = [];
                const result = verb === "set" ? way(o, 2) : way(o);
                const line = `${result} | ${before} -> ${state(o)}${seen.length ? " | " + seen.join(", ") : ""}`;
                if (!results.includes(line))
                    results.push(line);
            }
            print("    " + verb, "=>", results.join("  AND BY __dict__  "));
        }
        // That of the others cannot be changed at all.
        if (make !== m.make_function)
            continue;
        const o = prepare(make());
        print("    otherwise, by __dict__ =>", s(m.others_by_dict(o)), state(o));
    }
}
