//@ requireOptions("--useDollarVM=1")

// A whole float is the same number to JavaScript as the int32 with its value. Every operation here is done with a plain number
// and with the whole float, and what they give is compared. x is the subject, p is the same number and always plain, y is
// another plain number.
//
// And only a move keeps it a whole float: what an operator or Math gives is a plain number, in every tier.
const toFloat = $vm.tagged.toFloat, isWholeFloat = $vm.tagged.isWholeFloat;

const operations = `
x + y ;; y + x ;; x - y ;; y - x ;; x * y ;; y * x ;; x / y ;; y / (x + 3) ;; x % y ;; y % (x + 3) ;; x ** 2 ;; 2 ** (x & 7) ;; -x ;; +x ;; x + 0.5 ;; x * 0.5 ;; 0.5 - x ;; x / 0.5
x + "s" ;; "s" + x ;; \`a\${x}b\` ;; x + x ;; x * x ;; x - x ;; x + p ;; x - p ;; x * p
(() => { let v = x; v++; return v; })() ;; (() => { let v = x; return v++; })() ;; (() => { let v = x; --v; return v; })() ;; (() => { let v = x; v += y; return v; })() ;; (() => { let v = x; v *= 0.5; return v; })()
x | 0 ;; x | y ;; y | x ;; x & y ;; x & -1 ;; x & 255 ;; x ^ y ;; x ^ 0 ;; ~x ;; x << 1 ;; x << 0 ;; 1 << (x & 7) ;; x >> 1 ;; x >> 0 ;; x >>> 1 ;; x >>> 0 ;; y >> (x & 7) ;; x | x ;; x & x ;; x | p ;; x & p
x < y ;; x <= y ;; x > y ;; x >= y ;; y < x ;; y <= x ;; x < 0.5 ;; x > 0.5 ;; x < p ;; x <= p ;; x >= p ;; p <= x ;; x < "3" ;; x < x ;; x <= x
x == y ;; x != y ;; x == p ;; p == x ;; x != p ;; x == x ;; x == String(p) ;; x == null ;; x == undefined ;; x == true ;; x == false ;; x == 0.5 ;; x == p + 0.5 - 0.5 ;; x == BigInt(p) ;; x == { valueOf() { return p; } }
x === y ;; x !== y ;; x === p ;; p === x ;; x !== p ;; p !== x ;; x === x ;; x !== x ;; x === String(p) ;; x === null ;; x === undefined ;; x === true ;; x === 0.5 ;; x === p + 0.5 - 0.5 ;; p + 0.5 - 0.5 === x
(() => { if (x === p) return "same"; return "different"; })() ;; (() => { if (p !== x) return "different"; return "same"; })() ;; (() => { if (x == p) return "same"; return "different"; })() ;; (() => { let o = { v: x }; return o.v === p; })() ;; (() => { let a = [x, "s"]; return a[0] === p; })()
!x ;; !!x ;; x ? 1 : 2 ;; x && 1 ;; x || 1 ;; x ?? 1 ;; (() => { if (x) return "t"; return "f"; })() ;; (() => { let n = 0; for (let v = x; v; v = 0) n++; return n; })() ;; Boolean(x) ;; (() => { let o = { v: x }; if (o.v) return "t"; return "f"; })() ;; [x, "s"].filter(Boolean).length
typeof x ;; Number.isInteger(x) ;; Number.isSafeInteger(x) ;; Number.isFinite(x) ;; Number.isNaN(x) ;; isFinite(x) ;; isNaN(x) ;; Number(x) ;; parseInt(x) ;; parseFloat(x) ;; String(x) ;; x.toString() ;; x.toString(2) ;; x.toFixed(2) ;; x.toPrecision(12) ;; x.toExponential(2) ;; x.toLocaleString("en-US") ;; BigInt(x) + "" ;; Object(x).valueOf() ;; new Number(x) + 1 ;; x.valueOf() ;; Object(x) == p
JSON.stringify(x) ;; JSON.stringify([x]) ;; JSON.stringify({ a: x }) ;; JSON.stringify([x, "s"]) ;; JSON.stringify({ a: 1 }, null, x & 3) ;; JSON.parse(JSON.stringify([x]))[0]
Math.abs(x) ;; Math.floor(x) ;; Math.ceil(x) ;; Math.round(x) ;; Math.trunc(x) ;; Math.sign(x) ;; Math.sqrt(x & 255) ;; Math.max(x, y) ;; Math.max(y, x) ;; Math.min(x, y) ;; Math.max(x) ;; Math.max(x, 0.5) ;; Math.min(x, p) ;; Math.pow(x, 2) ;; Math.imul(x, y) ;; Math.clz32(x) ;; Math.fround(x) ;; Math.hypot(x, y) ;; Math.atan2(x, y) ;; Math.cbrt(x) ;; Math.log2(x & 255) ;; Math.max(...[x, y]) ;; Math.max.apply(null, [x, y])
[10, 20, 30, 40][x & 3] ;; [10, 20, 30, 40][x] ;; [10, "s", 30, 40][x] ;; [0.5, 1.5, 2.5][x] ;; "abcdefgh"[x] ;; "abcdefgh".charAt(x) ;; "abcdefgh".charCodeAt(x) ;; "abcdefgh".codePointAt(x) ;; "abcdefgh".at(x) ;; "abcdefgh".slice(x) ;; "abcdefgh".substring(x) ;; "abcdefgh".substr(x, 2) ;; "ab".padStart(x & 7) ;; "ab".repeat(x & 3) ;; "abcabc".indexOf("c", x) ;; String.fromCharCode(x & 127) ;; String.fromCodePoint(x & 127)
[1, 2, 3, 4].slice(x).join() ;; [1, 2, 3, 4].at(x) ;; [1, 2, 3, 4].splice(x, 1).join() ;; [1, 2, 3, 4].with(x & 3, 9).join() ;; [1, 2, 3, 4].fill(9, x).join() ;; [1, 2, 3, 4].fill(x).join() ;; [1, 2, 3, 4].copyWithin(0, x & 3).join() ;; new Array(x & 7).length ;; Array(x & 7).fill(0).length ;; Array.from({ length: x & 7 }).length ;; Array.of(x).join() ;; (() => { let a = [1, 2, 3]; a.length = x & 3; return a.length; })() ;; [[1, [2, [3]]]].flat(x & 3).length
(() => { let o = {}; o[x] = 1; return Object.keys(o).join(); })() ;; (() => { let o = {}; o[x] = 1; return o[p]; })() ;; (() => { let o = {}; o[p] = 1; return o[x]; })() ;; (() => { let a = []; a[x & 7] = 1; return a.length; })() ;; (x & 3) in [1, 2] ;; x in [1, 2, 3] ;; [1, 2, 3].hasOwnProperty(x) ;; Reflect.get([1, 2, 3], x) ;; Object.hasOwn([1, 2, 3], x) ;; ({ [x]: 1 })[p] ;; (() => { let a = [1, 2, 3]; delete a[x & 3]; return a.join(); })()
(() => { let t = new Int32Array(4); t[0] = x; return t[0]; })() ;; (() => { let t = new Float64Array(4); t[0] = x; return t[0]; })() ;; (() => { let t = new Uint8Array(4); t[0] = x; return t[0]; })() ;; (() => { let t = new Uint8ClampedArray(4); t[0] = x; return t[0]; })() ;; new Int32Array([5, 6, 7, 8])[x & 3] ;; new Int32Array([5, 6, 7, 8])[x] ;; new Int32Array(x & 7).length ;; new Int32Array(4).fill(x).join() ;; new Int32Array([p, 1]).indexOf(x) ;; new Int32Array([p, 1]).includes(x) ;; new Float64Array([p, 1]).indexOf(x) ;; new Int32Array([x, 1])[0] ;; Int32Array.of(x)[0] ;; Int32Array.from([x])[0] ;; (() => { let d = new DataView(new ArrayBuffer(8)); d.setInt32(0, x); return d.getInt32(0); })() ;; (() => { let d = new DataView(new ArrayBuffer(16)); d.setInt8(x & 7, 5); return d.getInt8(x & 7); })() ;; Atomics.store(new Int32Array(new SharedArrayBuffer(16)), 0, x) ;; Atomics.add(new Int32Array(new SharedArrayBuffer(16)), 0, x) ;; new ArrayBuffer(x & 31).byteLength ;; new Int32Array([1, 2, 3, 4]).subarray(x & 3).length
[p, 100, 200].indexOf(x) ;; [p, 100, 200].includes(x) ;; [100, p, 200].lastIndexOf(x) ;; [p, "s"].indexOf(x) ;; [p, "s"].includes(x) ;; [p, 0.5].indexOf(x) ;; [p, 0.5].includes(x) ;; [100, 200].indexOf(x) ;; [100, 200, p].indexOf(p, x & 1)
[x, 100, 200].indexOf(p) ;; [x, 100, 200].includes(p) ;; [100, x, 200].lastIndexOf(p) ;; [x, "s"].indexOf(p) ;; [x, "s"].includes(p) ;; [x, "s"].lastIndexOf(p) ;; [x, 0.5].indexOf(p) ;; [x, 0.5].includes(p) ;; [x].indexOf(x) ;; [x].includes(x) ;; [x, "s"].indexOf(x) ;; [x, 100].find(e => e === p) ;; [x, 100].findIndex(e => e === p) ;; [x, 100].findIndex(e => e == p) ;; [x, 100].some(e => e === p) ;; [x, 100].filter(e => e === p).length
(() => { let a = []; a.push(x); return a.indexOf(p); })() ;; (() => { let a = []; a.push(x); return a.includes(p); })() ;; (() => { let a = [1, 2]; a[0] = x; return a.indexOf(p); })() ;; (() => { let a = new Array(2); a[0] = x; a[1] = 1; return a.includes(p); })() ;; Array.of(x, 1).indexOf(p) ;; [1, 2].fill(x).indexOf(p) ;; [1].concat([x]).indexOf(p) ;; [...[x, 1]].indexOf(p) ;; Array.from([x, 1]).indexOf(p) ;; [x, 1].slice().indexOf(p) ;; [x, 1].map(e => e).indexOf(p) ;; [1, x].reverse().indexOf(p) ;; [1, 2].with(0, x).indexOf(p) ;; [1, 2].toSpliced(0, 1, x).indexOf(p)
new Map([[p, "v"]]).get(x) ;; new Map([[x, "v"]]).get(p) ;; new Map([[x, "v"]]).get(x) ;; new Map([[p, "v"]]).has(x) ;; new Map([[x, "v"]]).has(p) ;; new Map([[p, 1], [x, 2]]).size ;; new Map([[x, 1], [p, 2]]).size ;; new Map([[x, 1]]).delete(p) ;; new Map([[p, 1]]).delete(x) ;; [...new Map([[x, 1]]).keys()].join() ;; new Map([[p + 0.5 - 0.5, "v"]]).get(x)
new Set([p]).has(x) ;; new Set([x]).has(p) ;; new Set([x]).has(x) ;; new Set([p, x]).size ;; new Set([x, p]).size ;; new Set([x, x]).size ;; new Set([x]).delete(p) ;; [...new Set([x, p, y])].join() ;; new Set([x]).union(new Set([p])).size ;; new Set([x]).isSubsetOf(new Set([p])) ;; Object.keys(Object.groupBy([x, p], e => e)).length ;; Map.groupBy([x, p], e => e).size
(() => { switch (x) { case 0: return "zero"; case 1: return "one"; case 2: return "two"; case 7: return "seven"; case -1: return "minus"; default: return "other"; } })() ;; (() => { switch (x) { case "a": return "a"; case p: return "p"; default: return "other"; } })() ;; (() => { switch (p) { case "a": return "a"; case x: return "x"; default: return "other"; } })() ;; (() => { switch (x) { case 0.5: return "half"; case 2: return "two"; case 0: return "zero"; default: return "other"; } })() ;; (() => { switch (true) { case x === p: return "same"; default: return "other"; } })()
Object.is(x, p) ;; Object.is(p, x) ;; Object.is(x, x) ;; Object.is(x, y) ;; Object.is(x, -0) ;; Object.is(x, p + 0.5 - 0.5)
[3, x, 1].sort((a, b) => a - b).join() ;; [3, x, 1].sort().join() ;; [3, x, 1].toSorted((a, b) => b - a).join() ;; [x, "b", 1].sort().join() ;; [3, x, 1].join("-") ;; [3, x, 1].reduce((a, b) => a + b) ;; [3, x, 1].toString() ;; [x, 1].reverse().join() ;; [3, x, 1].map(e => e * 2).join() ;; Math.max(...[3, x, 1])
new Date(x).getTime() ;; new Date(2020, x & 7).getMonth() ;; new Date(0).setMilliseconds(x & 255) ;; (() => { let n = 0; for (let i = 0; i < (x & 15); i++) n += i; return n; })() ;; (() => { let n = 0; for (let i = x & 15; i > 0; i--) n++; return n; })() ;; (() => { let n = 0, i = x; while (i < p + 3) { i++; n++; } return n; })()
(() => { let r = /a/g; r.lastIndex = x & 3; return r.test("aaaa") + ":" + r.lastIndex; })() ;; (() => { let r = /a/y; r.lastIndex = x & 3; return r.exec("aaba") + ""; })() ;; (() => { let r = /a/g; r.lastIndex = x & 0; return r.test("ba") + ":" + r.lastIndex; })() ;; "aXbXc".split("X", x & 3).length
(function () { return typeof this; }).call(x) ;; (function () { "use strict"; return this === p; }).call(x) ;; (function () { return this == p; }).call(x) ;; (function (...a) { return a[0] + 1; })(x) ;; (function () { return arguments[0] + 1; })(x) ;; (function () { return arguments[x & 1]; })(5, 6) ;; (function (a = 9) { return a; })(x) + 0 ;; ((a, b) => a - b).bind(null, x)(y) ;; Reflect.apply((a) => a + 1, null, [x])
(() => { let { a } = { a: x }; return a + 1; })() ;; (() => { let [a] = [x]; return a + 1; })() ;; (() => { function* g() { yield x; } return g().next().value + 1; })() ;; (() => { class C { v = x; } return new C().v + 1; })() ;; (() => { let f = () => x; return f() + 1; })() ;; (() => { try { throw x; } catch (e) { return e + 1; } })() ;; (() => { let o = Object.freeze({ v: x }); return o.v + 1; })() ;; (() => { let o = {}; Object.defineProperty(o, "v", { value: x }); Object.defineProperty(o, "v", { value: p }); return o.v + 0; })() ;; (() => { let s = Symbol(x); return s.description; })() ;; Number.prototype.toString.call(x) ;; new Intl.NumberFormat("en-US").format(x) ;; x.constructor === Number ;; x instanceof Number ;; isNaN(x / x) ;; 1 / x
`.split(/\n| ;; /).map(s => s.trim()).filter(Boolean);

// Those that hand on x itself, and when.
const moves = {
    "+x": () => true,
    "(() => { let v = x; return v++; })()": () => true,
    "x && 1": v => !v,
    "x || 1": v => !!v,
    "x ?? 1": () => true,
    "Number(x)": () => true,
    "parseFloat(x)": () => true,
    "Math.max(x)": () => true,
    "[x, 100].find(e => e === p)": () => true,
};
for (let expression in moves) {
    if (!operations.includes(expression))
        throw new Error("Not among the operations: " + expression);
}

function canonical(r) {
    if (typeof r === "number")
        return "number:" + (Object.is(r + 0, -0) ? "-0" : String(r));
    if (typeof r === "object" && r !== null)
        return "object:" + JSON.stringify(r);
    return typeof r + ":" + String(r);
}

let made = 0;
function make(expression) {
    // Text that has not been seen before, so that nothing is remembered about it.
    let f = new Function("x", "p", "y", "/* " + (made++) + " */ return " + expression + ";");
    noInline(f);
    return f;
}
function run(f, x, p, y) {
    try { return f(x, p, y); } catch (e) { return "throws " + e.constructor.name; }
}

const values = [0, 1, 2, 7, -1, 100000, 2147483647, -2147483648];
const count = testLoopCount >> 2;

for (let expression of operations) {
    let plain = make(expression);
    let expected = new Map;
    for (let i = 0; i < count; ++i) {
        let v = values[i % values.length];
        expected.set(v, canonical(run(plain, v, v, 3)));
    }
    // "surprised": compiled for plain numbers and then given whole floats. "trained": has only ever seen whole floats.
    for (let training of ["surprised", "trained"]) {
        let f = make(expression);
        if (training === "surprised") {
            for (let i = 0; i < count; ++i) {
                let v = values[i % values.length];
                run(f, v, v, 3);
            }
        }
        for (let i = 0; i < count; ++i) {
            let v = values[i % values.length];
            let result = run(f, toFloat(v), v, 3);
            if (canonical(result) !== expected.get(v))
                throw new Error(expression + " of " + v + " gave " + canonical(result) + " and not " + expected.get(v) + " (" + training + ", call " + i + ")");
            let moved = expression in moves && moves[expression](v);
            if (isWholeFloat(result) !== moved)
                throw new Error(expression + " of " + v + " gave " + describe(result) + ", which should " + (moved ? "" : "not ") + "be a whole float (" + training + ", call " + i + ")");
        }
    }
}

// Whether it is true, at a place that has seen values of many types.
function ifTrue(v) { if (v) return "true"; return "false"; }
function not(v) { return !v; }
function conditional(v) { return v ? 1 : 2; }
noInline(ifTrue); noInline(not); noInline(conditional);
const mixed = ["", "a", 0, 1, {}, null, undefined, true, false, 0.5, 7];
for (let i = 0; i < testLoopCount * 2; ++i) {
    let v = mixed[i % mixed.length];
    ifTrue(v); not(v); conditional(v);
}
for (let i = 0; i < 100; ++i) {
    let shown = [ifTrue(toFloat(0)), not(toFloat(0)), conditional(toFloat(0)), ifTrue(toFloat(1)), not(toFloat(1)), conditional(toFloat(1))].join();
    if (shown !== "false,true,2,true,false,1")
        throw new Error("Whether 0.0 and 1.0 are true: " + shown);
}
