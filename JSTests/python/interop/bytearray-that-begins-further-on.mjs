// A bytearray is a Uint8Array. What Python takes from the front of a large one is not made up for by moving the rest: it begins further on in the memory that it has. To JavaScript that is a view that begins some
// way into its ArrayBuffer, which is what it turns out to be if the ArrayBuffer is asked for.
import * as m from "./bytearray-that-begins-further-on.py";
const show = (label, ...values) => print(label, "=>", values.map(v => Array.isArray(v) || ArrayBuffer.isView(v) ? "[" + Array.from(v).join(" ") + "]" : String(v)).join(" | "));
const expected = (from, to) => { let sum = 0; for (let i = from; i < to; ++i) sum += i & 255; return [to - from, from & 255, (to - 1) & 255, sum % 1000]; };

for (const size of [500, 1000, 1001, 5000, 100000]) {
    const a = m.make(size);
    show("new " + size, a instanceof Uint8Array, a.length, a.byteOffset, a.byteLength);
    m.drop(a, 100);
    show("a hundred taken", a.length, a.byteLength, a[0], a[a.length - 1], a.at(-1), Array.from(m.look(a)).join() === expected(100, size).join());
    // In code that has been compiled, too.
    const sum = v => { let s = 0; for (let i = 0; i < v.length; ++i) s += v[i]; return s; };
    let total = 0;
    for (let i = 0; i < 300; ++i)
        total = sum(a);
    m.drop(a, 50);
    show("fifty more", a.length, a[0], sum(a) % 1000 === expected(150, size)[3], total % 1000 === expected(100, size)[3]);
    show("parts of it", a.subarray(0, 3), a.slice(-2), a.indexOf(200), new Uint8Array(a).length, a.subarray(1).byteOffset - a.byteOffset);
    a[0] = 7;
    show("written to", Array.from(m.look(a))[1], a.fill(9, 0, 2)[1], Array.from(m.look(a))[1]);
    a[0] = 150 & 255;
    a[1] = 151 & 255;
}

// It is asked for its ArrayBuffer, which begins where the memory does.
{
    const a = m.make(5000);
    m.drop(a, 1200);
    const before = Array.from(a);
    const offset = a.byteOffset;
    const buffer = a.buffer;
    show("its ArrayBuffer", buffer.byteLength - offset === a.length, a.byteOffset === offset, a.length, Array.from(a).join() === before.join(), Array.from(new Uint8Array(buffer, a.byteOffset, a.length)).join() === before.join());
    new Uint8Array(buffer)[a.byteOffset] = 77;
    show("is the same memory", a[0], Array.from(m.look(a))[1]);
    show("and now it is not to change in length", m.attempt(m.drop, a, 10), m.attempt(m.add, a, [1]), a.length);
}

// Taken from and added to in turn, for long enough that what was given up has to be given back many times.
{
    const a = m.make(2000);
    let from = 0, to = 2000, right = true, most = 0;
    for (let round = 0; round < 3000; ++round) {
        const k = (round * 37) % 300;
        if (k <= to - from) {
            m.drop(a, k);
            from += k;
        }
        const data = [];
        for (let i = 0; i < (round * 53) % 290; ++i)
            data.push(to++ & 255);
        m.add(a, data);
        right &&= a.length === to - from && (!a.length || (a[0] === (from & 255) && a[a.length - 1] === ((to - 1) & 255)));
        most = Math.max(most, a.byteOffset / Math.max(a.length, 1000));
    }
    show("in turn", right, a.length === to - from, Array.from(m.look(a)).join() === expected(from, to).join(), "no more is given up than is kept:", most <= 1.5);
}
