// Each pattern: one get_by_id site inside a function, executed `iters` times.
// Prints ns per access. Run with --useJIT=0 to see pure LLInt behaviour.
const iters = (typeof ITERS !== "undefined") ? ITERS : 2_000_000;

function time(name, setup, body) {
    const ctx = setup();
    const start = preciseTime();
    let acc = 0;
    for (let i = 0; i < iters; i++)
        acc += body(ctx, i) | 0;
    const end = preciseTime();
    print(name.padEnd(34), ((end - start) * 1e9 / iters).toFixed(1).padStart(8), "ns/access", acc);
}

function makeShapes(n) {
    const out = [];
    for (let i = 0; i < n; i++) {
        const o = {};
        for (let j = 0; j < i; j++)
            o["pad" + j] = j;
        o.type = i;
        out.push(o);
    }
    return out;
}

function getType(o) { return o.type; }
function getType2(o) { return o.type; }
function getType4(o) { return o.type; }
function getType8(o) { return o.type; }
function getType32(o) { return o.type; }
function getMissing(o) { return o.missing === undefined ? 1 : 0; }
function getMissingPoly(o) { return o.missing === undefined ? 1 : 0; }
function getGetter(o) { return o.g; }
function getCustom(o) { return o.byteLength; }
function strLength(s) { return s.length; }
function mixedLength(s) { return s.length; }
function protoAfterOwn(o) { return o.m; }
function arrayBuiltin(a) { return a.indexOf === Array.prototype.indexOf ? 1 : 0; }
function protoMono(o) { return o.m; }
function numberProto(n) { return n.toFixed === undefined ? 0 : 1; }

time("own mono", () => makeShapes(1), (s, i) => getType(s[0]));
time("own poly 2", () => makeShapes(2), (s, i) => getType2(s[i & 1]));
time("own poly 4", () => makeShapes(4), (s, i) => getType4(s[i & 3]));
time("own poly 8", () => makeShapes(8), (s, i) => getType8(s[i & 7]));
time("own poly 32", () => makeShapes(32), (s, i) => getType32(s[i & 31]));
time("absent mono", () => ({ a: 1, b: 2 }), (o, i) => getMissing(o));
time("absent poly 2", () => [{ a: 1 }, { b: 2 }], (s, i) => getMissingPoly(s[i & 1]));
time("getter on proto", () => { class C { get g() { return 1; } } return new C; }, (o, i) => getGetter(o));
time("custom (ArrayBuffer.byteLength)", () => new ArrayBuffer(8), (o, i) => getCustom(o));
time("string length", () => ["a", "bb", "ccc", "dddd"], (s, i) => strLength(s[i & 3]));
time("string+array length", () => ["a", [1, 2], "ccc", [1]], (s, i) => mixedLength(s[i & 3]));
time("proto mono", () => { class C { m() { } } C.prototype.m = 5; return new C; }, (o, i) => protoMono(o));
time("proto after own", () => {
    class C { } C.prototype.m = 5;
    const own = { m: 1 };
    protoAfterOwn(own); // the site first sees an own-data hit
    return new C;
}, (o, i) => protoAfterOwn(o));
time("array builtin, 3 array shapes", () => [[1, 2], [1.5, 2.5], ["a", {}]], (s, i) => arrayBuiltin(s[i % 3]));
time("number receiver (non-cell)", () => 5, (n, i) => numberProto(n));
