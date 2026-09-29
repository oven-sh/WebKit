// A regular expression of Python's goes through the bytes of anything of JavaScript's that has bytes. JavaScript can do more to them than Python can: give the ArrayBuffer away, or resize it.
import * as m from "./regular-expressions-over-bytes-that-change.py";

function bytes(text, times = 1) { return Uint8Array.from(text.repeat(times), c => c.charCodeAt(0)); }
function show(label, f) {
    try {
        print(label, "=>", String(f()));
    } catch (e) {
        print(label, "=>", String(e));
    }
}

m.in_python();

const pattern = m.B.pattern;
show("a Uint8Array", () => m.search(pattern, bytes("abcb")));
show("part of one", () => m.search(pattern, bytes("bbabcb").subarray(2)));
show("an ArrayBuffer", () => m.search(pattern, bytes("abcb").buffer));
show("a DataView", () => m.search(pattern, new DataView(bytes("abcb").buffer, 1)));
show("a Uint16Array", () => m.findall(pattern, new Uint16Array([0x62, 0x6262])));
show("a string", () => m.search(pattern, "abcb"));
show("a number", () => m.search(pattern, 5));

for (const size of [1, 3, 40, 4000]) {
    show(`given away while it is replaced, ${size}`, () => {
        const subject = bytes("abcbd", size);
        return [String(m.sub(subject, () => { subject.buffer.transfer(); })).slice(0, 20), subject.length];
    });
    show(`given away while it is gone through, ${size}`, () => {
        const subject = bytes("abcbd", size);
        return [m.spans(subject, () => { subject.buffer.transfer(); }), subject.length];
    });
    show(`its ArrayBuffer is asked for, ${size}`, () => {
        const subject = bytes("abcbd", size);
        return String(m.sub(subject, () => { subject.buffer; })).length;
    });
    show(`made shorter, ${size}`, () => {
        const buffer = new ArrayBuffer(5 * size, { maxByteLength: 100000 });
        const subject = new Uint8Array(buffer);
        subject.set(bytes("abcbd", size));
        return [String(m.sub(subject, () => { buffer.resize(2); })).slice(0, 20), subject.length];
    });
    show(`made longer, ${size}`, () => {
        const buffer = new ArrayBuffer(5 * size, { maxByteLength: 100000 });
        const subject = new Uint8Array(buffer);
        subject.set(bytes("abcbd", size));
        return [String(m.sub(subject, () => { buffer.resize(50000); })).length, subject.length];
    });
    show(`written over, ${size}`, () => {
        const subject = bytes("abcbd", size);
        return String(m.sub(subject, () => { subject.fill(0x62); })).length;
    });
}
