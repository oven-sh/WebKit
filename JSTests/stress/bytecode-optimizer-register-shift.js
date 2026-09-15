//@ runDefault("--useBytecodeOptimizer=1")
//@ runDefault("--useBytecodeOptimizer=1", "--useConcurrentJIT=0", "--jitPolicyScale=0")
// Scope-resolution caching allocates fresh registers below all temporaries, shifting every temporary and every
// call frame offset (argv / stackOffset / firstFree). Exercise each frame-building instruction inside a closure two
// scopes below the variables it reads (so resolutions are cached): call with every argument-count parity,
// call_varargs, construct, construct_varargs, super calls, tail calls, iterator_open/iterator_next (generic
// iterators that throw or get closed), array/rest destructuring, strcat, new_array_with_spread, recursion, plus
// a function near the 4096-local limit.
const __expect = {"frames": "21,20,,10,10/11,21,101102,101102,1011/3110-15-11,{\"p\":10,\"q\":[12,12],\"s\":\"101134\"},11,11,11,10/11,23,21,2,12,53,2,11,12,33,nx210,0,1,12,1,10:11:inner:0,2,10111010,22,11,34,3 | 7,6,,3,3/4,7,3402,3402,34/103-8-4,{\"p\":3,\"q\":[5,5],\"s\":\"3434\"},4,4,4,3/4,9,7,3,5,18,2,5,5,18,nx23,0,1,5,3,3:4:inner:1,3,3433,9,4,34,3 | 27,20,,12,12/15,27,121504,121504,1215/3912-19-15,{\"p\":12,\"q\":[16,14],\"s\":\"121534\"},15,15,15,12/15,31,27,5,14,69,2,16,14,39,nx212,0,1,14,5,12:15:inner:3,5,12151010,31,13,4 | 7:7,8,9,10,11,12,13,7 | 0,1,7,2,z,,,7 | 7:8\n30,20,,13,13/17,31,131715,131715,1317/4313-21-17,{\"p\":13,\"q\":[18,15],\"s\":\"131734\"},17,17,17,13/17,35,30,2,15,77,2,14,15,58,nx213,0,1,15,6,13:17:inner:0,2,13171010,31,14,34,6 | 13,6,,5,5/8,14,5814,5814,58/185-12-8,{\"p\":5,\"q\":[9,7],\"s\":\"5834\"},8,8,8,5/8,17,13,3,7,34,2,7,7,26,nx25,0,1,7,8,5:8:inner:1,3,5833,15,6,34,5 | 36,20,,15,15/21,37,152117,152117,1521/5115-25-21,{\"p\":15,\"q\":[22,17],\"s\":\"152134\"},21,21,21,15/21,43,36,5,17,93,2,19,17,66,nx215,0,1,17,10,15:21:inner:3,5,15211010,40,16,7 | 1,7,9,,,,,7 | 1,2,7,10,z,,,7 | 7:16\n39,20,,16,16/23,41,162328,162328,1623/5516-27-23,{\"p\":16,\"q\":[24,18],\"s\":\"162334\"},23,23,23,16/23,47,39,2,18,101,2,17,18,51,nx216,0,1,18,11,16:23:inner:0,2,16231010,40,17,34,9 | 19,6,,7,7/12,21,71226,71226,712/267-16-12,{\"p\":7,\"q\":[13,9],\"s\":\"71234\"},12,12,12,7/12,25,19,3,9,50,2,9,9,34,nx27,0,1,9,13,7:12:inner:1,3,71233,21,8,34,7 | 45,20,,18,18/27,47,1827210,1827210,1827/6318-31-27,{\"p\":18,\"q\":[28,20],\"s\":\"182734\"},27,27,27,18/27,55,45,5,20,117,2,22,20,57,nx218,0,1,20,15,18:27:inner:3,5,18271010,49,19,10 | 7:7,8,9,10,11,12,13,7 | 2,3,7,18,z,,,7 | 7:24\n273,20,,94,94/179,301,941792886,941792886,94179/36794-183-179,{\"p\":94,\"q\":[180,96],\"s\":\"9417934\"},179,179,179,94/179,359,273,2,96,725,2,95,96,285,nx294,0,1,96,141,94:179:inner:0,2,941791010,274,95,34,87 | 175,6,,59,59/116,203,591162858,591162858,59116/23459-120-116,{\"p\":59,\"q\":[117,61],\"s\":\"5911634\"},116,116,116,59/116,233,175,3,61,466,2,61,61,242,nx259,0,1,61,143,59:116:inner:1,3,5911633,177,60,34,59 | 279,20,,96,96/183,307,961832888,961832888,96183/37596-187-183,{\"p\":96,\"q\":[184,98],\"s\":\"9618334\"},183,183,183,96/183,367,279,5,98,741,2,100,98,291,nx296,0,1,98,145,96:183:inner:3,5,961831010,283,97,88 | 7:7,8,9,10,11,12,13,7 | 28,29,7,226,z,,,7 | 7:232\n282,20,,97,97/185,311,971852989,971852989,97185/37997-189-185,{\"p\":97,\"q\":[186,99],\"s\":\"9718534\"},185,185,185,97/185,371,282,2,99,749,2,98,99,394,nx297,0,1,99,146,97:185:inner:0,2,971851010,283,98,34,90 | 181,6,,61,61/120,210,611202960,611202960,61120/24261-124-120,{\"p\":61,\"q\":[121,63],\"s\":\"6112034\"},120,120,120,61/120,241,181,3,63,482,2,63,63,250,nx261,0,1,63,148,61:120:inner:1,3,6112033,183,62,34,61 | 288,20,,99,99/189,317,991892991,991892991,99189/38799-193-189,{\"p\":99,\"q\":[190,101],\"s\":\"9918934\"},189,189,189,99/189,379,288,5,101,765,2,103,101,402,nx299,0,1,101,150,99:189:inner:3,5,991891010,292,100,91 | 29,7,233,,,,,7 | 29,30,7,234,z,,,7 | 7:240", "manyLocals": "100:1087309 120:1163713 4000:7934713 4090:7951172"}; let __checked = 0; function check(name, value) { if (!(name in __expect)) throw new Error("no expectation recorded for " + name); if (String(value) !== __expect[name]) throw new Error(name + ": expected " + JSON.stringify(__expect[name]) + " but got " + JSON.stringify(String(value))); __checked++; } const __expectedChecks = 2;

function mkIt(n, throwAt) { return { [Symbol.iterator]() { let i = 0; return { next() { if (i === throwAt) throw new Error("nx" + i); return { done: i >= n, value: i++ }; }, return() { mkIt.closed++; return {}; } }; } }; }
mkIt.closed = 0;
function id(x) { return x; } function two(a, b) { return a + "/" + b; } function three(a, b, c) { return a + b + c; } function four(a, b, c, d) { return "" + a + b + c + d; }
function outer(seed) {
    let a = seed, b = seed + 1, c = [seed], d = { seed };
    class Base { constructor(...xs) { this.xs = xs; this.ab = a + b; } m(p) { return p + a + b; } static s(...q) { return q.length + a; } }
    return (function middle() {
        let mm = 1;
        return function inner(x, ...rest) {
            mm++;
            const r = [];
            r.push(a + b, c[0] + d.seed, id(), id(a), two(a, b), three(a, b, x), four(a, b, x, mm), four(a, b, x, mm, a));
            r.push(three(id(a), two(id(b), three(a, id(b), a)), `${id(a)}-${three(1, id(b), 3)}-${b}`), JSON.stringify({ p: id(a), q: [id(b + 1), id(a + 2)], s: four(...[a, b, 3, 4]) }));
            r.push(Math.max(a, b, x), Math.max(...c, a, b, ...rest), id.apply(null, [b]), two.call(null, a, b), Reflect.apply(three, null, [a, b, mm]));
            r.push(new Base(a, b, x).ab, new Base(...c, ...rest, a).xs.length);
            class D extends Base { constructor(...ys) { super(...ys, a, b); this.n = ys.length + a; } m(p) { return super.m(p) + a + b; } }
            class E extends Base { constructor(p) { super(p, a); } }
            r.push(new D(x, a).n, new D(...rest).m(b), new E(b).xs.length, Base.s(...rest, a), D.s(a, b));
            let s = 0; for (const v of mkIt(3 + (a & 1))) s += v + a;
            r.push(s);
            try { for (const v of mkIt(5, 2)) s += v + b; } catch (e) { r.push(e.message + a); }
            for (const v of mkIt(9)) { if (v > a % 3) break; s += b; }
            const [p = a, q = b, ...more] = mkIt(4); r.push(p, q, more.length + a, mkIt.closed);
            r.push(`${a}:${b}:${inner.name}:${rest.length}`, [a, b, ...rest].length, [a, [b, [c[0], [d.seed]]]].flat(3).join(""));
            r.push(((...z) => z.length + a + b)(...c, ...rest), inner.length + a);
            if (rest.length < 2) r.push(inner(x + 1, ...rest, a, b).length); // recursion with a growing argument count
            a++; b += 2; r.push(mm);
            return r;
        };
    })();
}
function strictOuter(seed) {
    "use strict";
    let a = seed, b = [seed, seed + 1, seed + 2, seed + 3, seed + 4, seed + 5, seed + 6];
    function wide(p0, p1, p2, p3, p4, p5, p6) { return [p0, p1, p2, p3, p4, p5, p6, a].join(","); }
    return (function middle() {
        let mm = 0;
        return {
            tail(v) { mm++; const keep = { v, a }; if (v & 1) return wide(v, a, mm); return keep.a + ":" + wide.apply(null, b); },           // tail_call / tail_call_varargs growing the frame
            spreadTail(...args) { mm++; return wide(...args, a, mm, "z"); },
            chain(n) { mm++; return n <= 0 ? a + ":" + mm : this.chain(n - 1); },
        };
    })();
}
const f = outer(10), g = outer(3), st = strictOuter(7);
const rows = [];
for (let i = 0; i < 30; i++) {
    const row = [f(i).join(","), g(i, 1).join(","), f(i, 1, 2, 3).join(","), st.tail(i), st.spreadTail(i, i + 1), st.chain(5)].join(" | ");
    if (i < 3 || i > 27) rows.push(row);
}
check("frames", rows.join("\n"));

function build(n, refs) {
    let body = "";
    for (let i = 0; i < n; i++) body += `let l${i} = x + ${i};`;
    body += "let acc = 0;";
    for (let i = 0; i < refs; i++) body += `acc += oa * ${i} + ob + l${(i * 37) % n} + idc(l${(i * 11) % n}, acc, oa);`;
    body += `for (let i = 0; i < ${n}; i += 97) acc ^= l0; return acc + l${n - 1};`;
    return new Function("idc", `let oa = 2, ob = 3; return (function mid() { let mm = 0; return function probe(x) { mm++; ${body} }; })();`)((p, q, r) => p + (q & 7) + r);
}
const big = [];
for (const n of [100, 120, 4000, 4090]) { const fn = build(n, 40); let h = 0; for (let i = 0; i < 3; i++) h = (h * 13 + fn(i)) | 0; big.push(n + ":" + h); }
check("manyLocals", big.join(" "));

if (__checked !== __expectedChecks)
    throw new Error("expected " + __expectedChecks + " checks, ran " + __checked);
