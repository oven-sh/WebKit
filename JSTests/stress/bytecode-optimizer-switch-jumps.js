//@ runDefault("--useBytecodeOptimizer=1")
//@ runDefault("--useBytecodeOptimizer=1", "--useConcurrentJIT=0", "--jitPolicyScale=0")
// Jump re-emission: switch jump tables (dense with holes, sparse lists, char, string, mixed, >255 cases forcing
// wide operands, fallthrough and default-in-the-middle), constant-condition folding (falsy/truthy literal kinds,
// numeric comparisons on constants, NaN/-0/BigInt/template constants), jump threading through jmp chains and
// jmp->ret, labeled blocks, and long functions whose forward branches sit near the 8/16-bit offset boundaries
// while dead code removal and operand widening change instruction sizes (jump-size relaxation).
const __expect = {"switches": "a,d0,dd0,bc,one,one,?c,two-seven,dd2,fg,d3,dd3,?e,four,dd4,?g,d5,dd5,de,d6,dd6,de,d-1,dd-1,de,d1.5,dd1.5,de,d1,dd1,de,d2147483648,dd2147483648,k,d1000,neg,d-5,big,d77777,max,d2147483647,dd-2147483648,d-2147483648,ddNaN,dNaN,dd0,d0,1,7,2,-1,26,-1,-1,-1,0,-1,0,9,0,-1,233,-1,0,-1,1,5,3,9,-1,-1,7,-1,n1,s1,f,null,u,t,def,zero,zero,def,0p,after0,0other,after1,after1,after1,1three,after2,notstring,after5,0,3,765,768,1797,-1,-1,-1,-1,1,8,13,-1,-1,-1,44851,44851,40846,1,1,1,036,d,369,294297d,297d,d,6912 ## a,d0,dd0,bc,one,one,?c,two-seven,dd2,fg,d3,dd3,?e,four,dd4,?g,d5,dd5,de,d6,dd6,de,d-1,dd-1,de,d1.5,dd1.5,de,d1,dd1,de,d2147483648,dd2147483648,k,d1000,neg,d-5,big,d77777,max,d2147483647,dd-2147483648,d-2147483648,ddNaN,dNaN,dd0,d0,1,7,2,-1,26,-1,-1,-1,0,-1,0,9,0,-1,233,-1,0,-1,1,5,3,9,-1,-1,7,-1,n1,s1,f,null,u,t,def,zero,zero,def,0p,after0,0other,after1,after1,after1,1three,after2,notstring,after5,0,3,765,768,1797,-1,-1,-1,-1,2,9,14,0,0,0,44851,44851,40846,1,1,1,036,d,369,294297d,297d,d,6912", "constants": "2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,1,0,1,0,0,0,dflt,,string,1,1 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,2,1,2,1,1,1,dflt,,string,2,2 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,3,2,3,2,2,2,dflt,,string,3,4 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,4,3,4,3,3,3,dflt,,string,4,7 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,5,4,5,4,4,4,dflt,,string,5,11 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,6,5,6,5,5,5,dflt,,string,6,16 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,7,6,7,6,6,6,dflt,,string,7,22 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,8,7,8,7,7,7,dflt,,string,8,29 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,9,8,9,8,8,8,dflt,,string,9,1 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,10,9,10,9,9,9,dflt,,string,10,2 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,11,10,11,10,10,10,dflt,,string,11,4 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,12,11,12,11,11,11,dflt,,string,12,7 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,13,12,13,12,12,12,dflt,,string,13,11 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,14,13,14,13,13,13,dflt,,string,14,16 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,15,14,15,14,14,14,dflt,,string,15,22 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,16,15,16,15,15,15,dflt,,string,16,29 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,17,16,17,16,16,16,dflt,,string,17,1 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,18,17,18,17,17,17,dflt,,string,18,2 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,19,18,19,18,18,18,dflt,,string,19,4 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,20,19,20,19,19,19,dflt,,string,20,7 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,21,20,21,20,20,20,dflt,,string,21,11 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,22,21,22,21,21,21,dflt,,string,22,16 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,23,22,23,22,22,22,dflt,,string,23,22 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,24,23,24,23,23,23,dflt,,string,24,29 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,25,24,25,24,24,24,dflt,,string,25,1 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,26,25,26,25,25,25,dflt,,string,26,2 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,27,26,27,26,26,26,dflt,,string,27,4 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,28,27,28,27,27,27,dflt,,string,28,7 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,29,28,29,28,28,28,dflt,,string,29,11 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,30,29,30,29,29,29,dflt,,string,30,16 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,31,30,31,30,30,30,dflt,,string,31,22 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,32,31,32,31,31,31,dflt,,string,32,29 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,33,32,33,32,32,32,dflt,,string,33,1 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,34,33,34,33,33,33,dflt,,string,34,2 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,35,34,35,34,34,34,dflt,,string,35,4 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,36,35,36,35,35,35,dflt,,string,36,7 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,37,36,37,36,36,36,dflt,,string,37,11 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,38,37,38,37,37,37,dflt,,string,38,16 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,39,38,39,38,38,38,dflt,,string,39,22 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,40,39,40,39,39,39,dflt,,string,40,29 ## 2,3,5,7,9,11,12,bigheap,13,15,16,17,18,20,nan-ne,21,23,24,strcmp,nonum,25,26,27,28,30,32,33,34,35,l,notnum,s,s1,s,s,s,dflt,,string,notnum,1", "longFunctions": "10/40:860429058 10/90:1126216133 100/60:1249451927 120/130:1234158604 260/120:542441455 600/40:1182679906"}; let __checked = 0; function check(name, value) { if (!(name in __expect)) throw new Error("no expectation recorded for " + name); if (String(value) !== __expect[name]) throw new Error(name + ": expected " + JSON.stringify(__expect[name]) + " but got " + JSON.stringify(String(value))); __checked++; } const __expectedChecks = 3;

function dense(x) { let r = "?"; switch (x) { case 0: r = "a"; break; case 1: r = "b"; case 2: r += "c"; break; default: r = "d"; case 4: r += "e"; break; case 3: r = "f"; case 5: r += "g"; } return r; }
function holes(x) { switch (x) { case 1: return "one"; case 4: return "four"; case 9: return "nine"; case 10: return "ten"; case 2: case 7: return "two-seven"; default: return "d" + x; } }
function sparse(x) { switch (x) { case 1: return "one"; case 1000: return "k"; case -5: return "neg"; case 77777: return "big"; case 2147483647: return "max"; default: return "dd" + x; } }
function chars(c) { switch (c) { case "a": return 1; case "b": return 2; case "z": return 26; case "A": return -1; case "é": return 233; default: return 0; } }
function strs(s) { let k = 0; switch (s) { case "alpha": k = 1; break; case "beta": k = 2; case "gamma": k += 3; break; case "": k = 9; break; case "a": k = 7; break; default: k = -1; } return k; }
function mixed(x) { switch (x) { case 1: return "n1"; case "1": return "s1"; case 1.5: return "f"; case null: return "null"; case undefined: return "u"; case true: return "t"; case 0: return "zero"; case -0: return "negzero"; default: return "def"; } }
function nested(x, y) { outer: switch (x & 3) { case 0: switch (y) { case "p": return "0p"; case "q": break outer; default: break; } return "0other"; case 1: for (let i = 0; i < 3; i++) { switch (i + y.length) { case 1: continue; case 2: break outer; case 3: return "1three"; } } return "1end"; default: { switch (typeof y) { case "string": break; default: return "notstring"; } } } return "after" + x; }
let src = "let r;switch(x){"; for (let i = 0; i < 600; i++) src += `case ${i}: r=${i * 3}; break;`; src += "default: r=-1;} return r;";
const big = new Function("x", src);
let src2 = "let r;switch(x){"; for (let i = 0; i < 400; i++) src2 += `case ${JSON.stringify("k" + i)}: r=${i}; ${i % 7 ? "break;" : ""}`; src2 += "default: r=-1;} return r+y;";
const bigStrings = new Function("x", "y", src2);
let src3 = "let r=0;switch(x){"; for (let i = 0; i < 300; i++) src3 += `case ${JSON.stringify(String.fromCharCode(33 + i % 90) + (i >= 90 ? String(i) : ""))}: r+=${i};`; src3 += "default: r+=1;} return r;";
const bigChars = new Function("x", src3);
let src4 = "let r=[];switch(x|0){"; for (let i = 0; i < 300; i += 3) src4 += `case ${i}: r.push(${i}); if (r.length > 2) break;`; src4 += "default: r.push('d');} return r.join('');";
const bigHoles = new Function("x", src4);

const out = new Set;
for (let i = 0; i < 40; i++) {
    const row = [];
    for (const v of [0, 1, 2, 3, 4, 5, 6, -1, 1.5, "1", 2147483648]) row.push(dense(v), holes(v), sparse(v));
    for (const v of [1000, -5, 77777, 2147483647, -2147483648, NaN, -0]) row.push(sparse(v), holes(v));
    for (const v of ["a", "b", "z", "A", "ab", "", 5, "é", "é"]) row.push(chars(v), strs(v));
    for (const v of ["alpha", "beta", "gamma", "", 5, "delta", "a", new String("alpha")]) row.push(strs(v));
    for (const v of [1, "1", 1.5, null, undefined, true, {}, 0, -0, 0n]) row.push(mixed(v));
    for (const [x, y] of [[0, "p"], [0, "q"], [0, "z"], [1, ""], [1, "a"], [1, "ab"], [1, "abc"], [2, "s"], [3, 4], [5, "q"]]) row.push(nested(x, y));
    for (const v of [0, 1, 255, 256, 599, 600, -1, "5", 2.5]) row.push(big(v));
    for (const v of ["k0", "k7", "k13", "k399", "k400", 7]) row.push(bigStrings(v, i & 1));
    for (const v of ["!", "\"", "!90", "{299", "nope", ""]) row.push(bigChars(v));
    for (const v of [0, 1, 3, 294, 297, 299, "6"]) row.push(bigHoles(v));
    out.add(row.join());
}
check("switches", [...out].join(" ## "));

function constants(v) {
    const r = [];
    if ("") r.push(1); else r.push(2);
    if ("0") r.push(3);
    while (0) { r.push(4); }
    do { r.push(5); } while (false);
    if (NaN) r.push(6); else r.push(7);
    if (-0) r.push(8); else r.push(9);
    if (0n) r.push(10); else r.push(11);
    if (1n) r.push(12);
    if (10n ** 30n) r.push("bigheap");
    if (`x`) r.push(13);
    if (`${""}`) r.push(14); else r.push(15);
    if (null == undefined) r.push(16);
    if (void 0 === undefined) r.push(17);
    const k = 5; if (k === k) r.push(18);
    const n = NaN; if (n === n) r.push(19); else r.push(20); if (n !== n) r.push("nan-ne");
    if (1 < 2) r.push(21); if (2 <= 1) r.push(22); else r.push(23);
    if ("a" < "b") r.push(24); if ("10" < "9") r.push("strcmp"); if ("10" < 9) r.push("numcmp"); else r.push("nonum");
    if (0 === -0) r.push(25); if (Object.is(0, -0)) r.push("is");
    if (Infinity > 1e308) r.push(26); if (-Infinity >= -Infinity) r.push(27);
    const u = undefined; if (u == null) r.push(28); if (u === null) r.push(29); if (u ?? true) r.push(30);
    const z = 0; if (z) r.push(31); else if (z == "") r.push(32);
    const s = "str"; if (s) r.push(33); if (s == "str") r.push(34); if (s.length === 3 === true) r.push(35);
    let x; if (true) { x = v + 1; } else { x = v.foo.bar(); }
    for (;;) { if (1) break; x = 99; }
    lbl: { if (typeof v === "number") break lbl; x = "notnum"; }
    l1: l2: for (;;) { r.push("l"); break l1; }
    const a = true ? v : v(), b = false ? v() : v + 1, c = null ?? v, d = 0 || v, e = 1 && v, f = undefined?.x ?? "dflt";
    r.push(x, a, b, c, d, e, f, void x, typeof typeof x, (1, 2, x));
    let i = 0, acc = 0; while (true) { acc += i; if (++i > (v & 7)) break; } do { acc++; } while (0 > 1); for (let j = 0; false;) acc = 1e9;
    r.push(acc);
    return r.join();
}
const constantRows = new Set;
for (let i = 0; i < 40; i++) constantRows.add(constants(i));
constantRows.add(constants("s"));
check("constants", [...constantRows].join(" ## "));

// Long functions: many forward branches over bodies that shrink (dead code) or grow (operand widening).
function makeLong(nLocals, reps, useOuter) {
    let s = "";
    for (let i = 0; i < nLocals; i++) s += `let l${i} = (x + ${i}) | 0;\n`;
    s += "let acc = 0;\n";
    for (let r = 0; r < reps; r++) {
        s += `if ((x + ${r}) & 1) { acc += l${r % nLocals}; ${useOuter ? "acc += oa + ob;" : ""} if (0) { acc = acc.no.such.thing(l0, l1, l2, l3); acc++; acc--; } } else { acc -= l${(r * 7) % nLocals} ^ ${r}; }\n`;
        if (r % 17 == 0) s += `for (let k = 0; k < 2; k++) { acc = (acc + k + ${useOuter ? "oa" : "1"}) | 0; if (acc & 4096) break; if ("") acc = -1; }\n`;
        if (r % 23 == 0) s += `switch ((x + ${r}) & 3) { case 0: acc++; break; case 1: acc += 2; case 2: acc ^= 7; break; default: acc--; }\n`;
        if (r % 29 == 0) s += `try { if ((acc & 63) == ${r & 63}) throw acc; acc += 3; } catch (e) { acc = (e + 11) | 0; }\n`;
    }
    s += "while ((x & 7) != 0) { x = (x + 1) | 0; acc = (acc * 3 + x) | 0; if (acc > 1e8) break; }\n";
    for (let i = 0; i < nLocals; i += 13) s += `acc ^= l${i};\n`;
    s += "return acc;";
    return new Function(`let oa = 3, ob = 4; return (function mid() { let mm = 0; return function probe(x) { mm++; ${s} }; })();`)();
}
const longResults = [];
for (const [nLocals, reps, outer] of [[10, 40, false], [10, 90, true], [100, 60, true], [120, 130, true], [260, 120, true], [600, 40, false]]) {
    const probe = makeLong(nLocals, reps, outer);
    let h = 0;
    for (let i = 0; i < 6; i++) h = (h * 31 + probe(i)) | 0;
    longResults.push(nLocals + "/" + reps + ":" + h);
}
check("longFunctions", longResults.join(" "));

if (__checked !== __expectedChecks)
    throw new Error("expected " + __expectedChecks + " checks, ran " + __checked);
