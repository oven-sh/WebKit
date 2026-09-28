import m from "./prototypes.py";
function show(label, f) { try { print(label, "=>", f()); } catch (e) { print(label, "!!", e.name + ":", String(e.message).slice(0, 100)); } }
const types = ["dict", "MyDict", "set", "frozenset", "tuple", "range", "slice", "complex", "property", "staticmethod", "super", "list_iterator", "dict_keys", "memoryview", "ValueError", "type", "int", "str", "bytes", "bytearray", "list", "MyList", "function", "generator", "Plain", "Slotted", "enumerate", "object"];

// Something of JavaScript's that is given a class for a prototype is no instance of it.
show("Object.create", () => types.map(n => m.name_of(Object.create(m[n]))).join(" "));
show("setPrototypeOf", () => types.map(n => m.name_of(Object.setPrototypeOf({}, m[n]))).join(" "));
show("of a function", () => types.map(n => m.name_of(Object.setPrototypeOf(function () {}, m[n]))).join(" "));
show("of an array", () => types.map(n => m.name_of(Object.setPrototypeOf([], m[n]))).join(" "));
show("of a Map", () => types.map(n => m.name_of(Object.setPrototypeOf(new Map, m[n]))).join(" "));
for (const [type, method, ...rest] of [["dict", "get", 1], ["dict", "keys"], ["dict", "__len__"], ["set", "add", 1], ["tuple", "index", 1], ["range", "count", 1], ["slice", "indices", 1], ["complex", "conjugate"], ["property", "getter", null],
        ["list_iterator", "__next__"], ["dict_keys", "isdisjoint", []], ["memoryview", "tobytes"], ["ValueError", "with_traceback", null], ["list", "append", 1], ["MyList", "append", 1], ["bytes", "hex"], ["str", "upper"], ["int", "bit_length"], ["Plain", "hello"], ["type", "mro"], ["generator", "send", null], ["function", "__call__"]]) {
    show(`${type}.${method}(fake)`, () => String(m[type][method](Object.create(m[type]), ...rest)).slice(0, 30));
}
show("what it inherits is the class's", () => { const o = Object.create(m.Plain); return [o.kind, typeof o.hello, o.make(), "kind" in o, o.nothing, Object.keys(o).length].join(" "); });
show("and it is its own to change", () => { const o = Object.create(m.Plain); o.kind = "mine"; o.fresh = 1; return [o.kind, m.Plain.kind, o.fresh, Object.keys(o).join(), delete o.kind, o.kind].join(" "); });
show("further down", () => { const o = Object.create(Object.create(m.Plain)); return [o.kind, o.make(), m.name_of(o)].join(" "); });

// What is Python's keeps its class.
for (const name of Object.keys(Object.fromEntries(m.samples.items()))) {
    const o = m.samples.get(name);
    show("swap " + name, () => { for (const t of types) { let threw = false; try { Object.setPrototypeOf(o, m[t]); threw = Object.getPrototypeOf(o) === m[t]; } catch { threw = true; } if (!threw) return "changed to " + t; } return m.name_of(o); });
}
show("to null", () => Object.setPrototypeOf(m.samples.get("dict"), null));
show("to Object.prototype", () => Object.setPrototypeOf(m.samples.get("Plain"), Object.prototype));
show("__proto__", () => { const o = m.samples.get("Plain"); o.__proto__ = m.Other; return [m.name_of(o), o.hello()].join(" "); });
show("Reflect.setPrototypeOf", () => Reflect.setPrototypeOf(m.samples.get("Plain"), m.Other));
show("to what it is", () => Object.setPrototypeOf(m.samples.get("Plain"), m.Plain) === m.samples.get("Plain"));
show("of a class", () => Object.setPrototypeOf(m.Plain, m.Other));
show("of a class to null", () => Reflect.setPrototypeOf(m.Plain, null));

// Python may change it, having checked.
show("__class__", () => { const o = m.Plain(); return [m.set_class(o, m.Other), o.hello(), Object.getPrototypeOf(o) === m.Other, o instanceof m.Other, o instanceof m.Plain].join(" "); });
show("__class__ that will not do", () => m.set_class(m.Plain(), m.Slotted));
show("__class__ of a dict", () => m.set_class(m.MyDict(), m.Plain));
show("__class__ built in", () => m.set_class(m.Plain(), m.dict));

// A constructor of JavaScript's, told to make one of Python's.
for (const [label, make] of [["Object/Plain", () => Reflect.construct(Object, [], m.Plain)], ["Map/dict", () => Reflect.construct(Map, [], m.dict)], ["Set/set", () => Reflect.construct(Set, [], m.set)], ["Array/list", () => Reflect.construct(Array, [], m.list)], ["Array/MyList", () => Reflect.construct(Array, [], m.MyList)],
        ["Uint8Array/bytes", () => Reflect.construct(Uint8Array, [2], m.bytes)], ["Uint8Array/bytearray", () => Reflect.construct(Uint8Array, [2], m.bytearray)], ["Error/ValueError", () => Reflect.construct(Error, ["x"], m.ValueError)], ["Function/function", () => Reflect.construct(Function, ["return 1"], m.function)],
        ["Promise/Plain", () => Reflect.construct(Promise, [() => {}], m.Plain)], ["Date/int", () => Reflect.construct(Date, [0], m.int)], ["String/str", () => Reflect.construct(String, ["s"], m.str)], ["Number/float", () => Reflect.construct(Number, [1], m.float)]]) {
    show(label, () => { const o = make(); return [m.name_of(o), m.repr(o).slice(0, 40)].join(" "); });
}
