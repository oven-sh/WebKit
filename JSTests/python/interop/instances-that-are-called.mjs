// An instance of a class that has __call__() is a function to JavaScript, whatever else the class is derived from.
import * as m from "./instances-that-are-called.py";

function show(label, f) {
    try {
        print(label, "=>", String(f()));
    } catch (e) {
        print(label, "=>", "JavaScript caught", String(e));
    }
}

print("---- with __call__()");
for (const x of m.with_call) {
    show(m.name(x), () => [typeof x, x(), x(1, "a"), Reflect.apply(x, undefined, [5]), [6].map(v => x(v)), Function.prototype.call.call(x, null, 7), m.call(x, 8), m.with_keywords(x), m.can_be_called(x)].join(" | "));
    show("    as a constructor", () => new x());
}
// It is a function, and is not derived from Function.
show("what Function.prototype has", () => m.with_call.map(x => [x.call, x.apply, x.bind, x instanceof Function].join()).filter(s => s !== ",,,false").length);
show("still what they were", () => [Array.isArray(m.with_call[8]), m.with_call[8].length, m.with_call[5] instanceof Uint8Array, Error.isError(m.with_call[12]), m.with_call[12].message]);
show("given to what wants a function", () => [[3, 1, 2].map(m.with_call[7]).length, Promise.resolve(1).then(m.with_call[9]) instanceof Promise, [1].filter(m.with_call[8]).length]);

print("---- derived, and without");
for (const x of m.without)
    show(m.name(x), () => [typeof x, m.can_be_called(x)]);
show("called", () => m.without[7]());
show("called by Python", () => m.call(m.without[7]));

print("---- what is built in");
for (const x of m.built_in)
    show(m.name(x), () => [typeof x, m.can_be_called(x)]);
show("called", () => m.built_in[0]());
show("called by Python", () => m.call(m.built_in[0]));

print("---- given to the class afterwards, and taken away");
show("before", () => [typeof m.later, m.can_be_called(m.later)]);
show("called", () => m.later(1));
m.give();
show("after", () => [typeof m.later, m.can_be_called(m.later), m.later(1)]);
m.take();
show("and after that", () => [typeof m.later, m.can_be_called(m.later)]);
show("called", () => m.later(1));

print("---- back and forth");
show("what it raises", () => new m.Raises()());
show("it calls a function of JavaScript's", () => m.Calls([3, 4])((...a) => a.join("+"), 1, 2));
show("which calls it", () => { const c = m.Calls([0]); return c((f, n) => n ? "done" : f((g, k) => "inner " + k, 1), c); });

print("---- what a constructor of JavaScript's made");
for (const x of m.made_by_javascript) {
    show(m.name(x), () => [typeof x, m.can_be_called(x), m.call(x), m.call(x, 1), m.with_keywords(x)].join(" | "));
    show("    called by JavaScript", () => x());
}
