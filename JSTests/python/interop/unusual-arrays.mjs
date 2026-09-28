// A list is an Array, and JavaScript can make arrays whose elements are not simply there to be read or written.
import { look, first, total } from "./unusual-arrays.py";
const each = text => text.split("; ").join("\n    ");
const show = (label, array) => print(label + "\n    " + each(look(array)));
show("plain", [1, 2, 3]);
show("frozen", Object.freeze([1, 2, 3]));
show("sealed", Object.seal([1, 2, 3]));
show("not extensible", Object.preventExtensions([1, 2, 3]));
const sparse = [1, 2, 3]; sparse[100000] = 4; sparse.length = 3;
show("was sparse", sparse);
const accessor = [1, 2, 3]; Object.defineProperty(accessor, 1, { get() { return 2; }, configurable: true });
show("has a getter", accessor);
show("has a hole", [1, , 3]);
const readOnly = [1, 2, 3]; Object.defineProperty(readOnly, 0, { writable: false });
show("one element cannot be written", readOnly);

const throws = [1, 2, 3]; Object.defineProperty(throws, 0, { get() { throw new RangeError("from the getter"); } });
for (const f of [first, total]) {
    try { print(f(throws)); } catch (e) { print(e.name + ": " + e.message); }
}
let written = [];
const setter = [1, 2, 3]; Object.defineProperty(setter, 0, { get() { return "got"; }, set(v) { written.push(v); } });
print(first(setter));
// The strings that a tag function is given are frozen.
const tag = strings => first(strings);
print(tag`first${0}second`);
