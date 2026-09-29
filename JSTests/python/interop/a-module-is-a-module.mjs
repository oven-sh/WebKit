// A module of JavaScript's is a module to Python: its namespace object, as it is.
import * as ns from "./a-module-of-javascript.mjs";
import * as middle from "./a-module-in-the-middle.mjs";
import * as aClass from "./a-module-whose-default-is-a-class.mjs";
import * as notCallable from "./a-module-whose-default-is-not-callable.mjs";
import * as noDefault from "./a-module-with-no-default.mjs";
import * as ofPython from "./a-module-whose-default-is-of-python.mjs";
import m from "./a-module-is-a-module.py";
const each = (label, list) => { print("---- " + label); for (const line of list) print("  " + line); };
const toJavaScript = () => JSON.stringify([Reflect.ownKeys(ns).map(String), Object.getOwnPropertyNames(ns), Object.keys(ns), Object.getOwnPropertySymbols(ns).map(String), (() => { const all = []; for (const k in ns) all.push(k); return all; })(),
    Object.keys({ ...ns }), Object.keys(Object.getOwnPropertyDescriptors(ns)), Object.isExtensible(ns), Object.isSealed(ns), Object.isFrozen(ns), String(ns[Symbol.toStringTag])]);
const before = toJavaScript();
each("what it is", m.what_it_is(ns));
each("what it has", m.what_it_has(ns));
each("what it has not", m.what_it_has_not(ns));
each("set and delete", m.set_and_delete(ns));
each("imported", m.imported(ns));
each("called", m.called(ns, aClass, notCallable, noDefault, ofPython));
print("---- to JavaScript");
print("  it is not to be called:", typeof ns, (() => { try { return ns("you"); } catch (e) { return e.constructor.name + ": " + e.message; } })(), (() => { try { return new ns(); } catch (e) { return e.constructor.name; } })());
print("  the same one:", m.give_back(ns) === ns);
print("  as it was:", toJavaScript() === before, before);
print("  what Python has set:", JSON.stringify(["by_dict", "__name__"].map(k => [ns[k], k in ns, Object.hasOwn(ns, k), Object.getOwnPropertyDescriptor(ns, k), Reflect.has(ns, k), delete ns[k], Reflect.set(ns, k, 1), Reflect.defineProperty(ns, k, { value: 1 })])));
each("and still has", [m.said(() => ns), m.said(() => m.give_back(ns).by_dict)]);
each("in the middle", middle.seen);
each("afterwards", m.afterwards(middle));
