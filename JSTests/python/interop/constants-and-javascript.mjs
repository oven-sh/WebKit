// A constant of Python code that is an object is made once. JavaScript is handed it like anything else, and is not to be able to make the code come to something else the next time that it is run.
import * as m from "./constants-and-javascript.py";

function attempt(f) {
    try {
        return String(f());
    } catch (e) {
        return `refused: ${e.message}`;
    }
}

// JavaScript can write to a bytes, which is a Uint8Array. So there is another each time.
const b = m.some_bytes();
b[0] = 120;
b.fill(121, 1);
print("bytes, written to:", String(b), "and the next:", String(m.some_bytes()), b === m.some_bytes());
const inTuple = m.bytes_in_a_tuple();
inTuple[0][0] = 120;
print("in a tuple:", String(inTuple), "and the next:", String(m.bytes_in_a_tuple()));
const given = m.some_bytes();
given.buffer.transfer();
print("given away:", given.length, "and the next:", String(m.some_bytes()));

// The rest are the same each time, and cannot be changed.
const t = m.a_tuple();
print("a tuple is one:", t === m.a_tuple(), Array.from(m.constants()).includes(t));
print("written to:", attempt(() => { t[0] = 5; return t[0]; }), attempt(() => { t.length = 0; return t.length; }), attempt(() => { t[2] = 5; return t[2]; }), attempt(() => Object.defineProperty(t, 0, { value: 5 })), attempt(() => delete t[0]),
    attempt(() => Array.prototype.reverse.call(t)), attempt(() => Array.prototype.fill.call(t, 5)), attempt(() => { t.extra = 1; return t.extra; }), attempt(() => Object.setPrototypeOf(t, null)));
print("and the next:", String(m.a_tuple()), m.a_tuple().extra);
const l = m.a_list();
l.push(4000);
l[0] = 5;
print("a list is another each time:", String(l), String(m.a_list()), l === m.a_list());
print("what is looked in:", m.a_frozenset(1000), m.a_frozenset(3000));
print("a slice of an array of JavaScript's:", String(m.a_slice([1, 2, 3])), String(m.a_slice([4, 5, 6])));
const c = m.a_complex();
print("a complex is one:", c === m.a_complex(), attempt(() => { c.real = 5; return c.real; }), String(m.a_complex()));
