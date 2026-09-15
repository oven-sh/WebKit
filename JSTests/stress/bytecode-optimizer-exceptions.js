//@ runDefault("--useBytecodeOptimizer=1")
//@ runDefault("--useBytecodeOptimizer=1", "--useConcurrentJIT=0", "--jitPolicyScale=0")
// Values flowing from try blocks into catch/finally/after, throws from getters/valueOf/toString in the middle of
// expressions (partially evaluated operands must keep their pre-throw values in the handler), finally overriding
// completions, nested handlers, and error.stack line:column / message text after re-emission (ExpressionInfo
// rebuild). Expected values were recorded from an unoptimized run of this exact file layout.
const __expect = {"row0": "3bbnt|3bb 383 0r0nt;0;0nt; iaf1c 0 E:3 1:10 1,0,init,vo 1,0,init,ts 1,init,ts 26RangeError sx2sy0 object0,nt,z 1fn2fn3fnf4 o10.20.|20.40.| bottom0 / iaf1cf2,100,2,2", "row1": "4b211|4b21 258 3r0nt;1;2c; iaf1ret 1 E:4 112:11 2,2,init,vo 2,2,init,ts 2,init,ts 27RangeError sx2sy1 objectrt1 c1f2fn3fnf4 o10.20.|20.40.| bottom0 / iaf1f2,101,3,2", "row2": "3bbnt|3bb 441 6r0nt;2;4nt; iaf1c 2 E:5 3:10 3,4,init,vo 3,4,init,ts 3,init,ts 28RangeError sx2sy2 object2,nt,z 1fnc2f3fnf4 o10.20.|20.40.| bottom0 / iaf1cf2,102,4,2", "row3": "4b233|4b23 260 9r0nt;3;6c; iaf1ret 3 E:6 14:111 4,6,init,vo 4,6,init,ts 4,init,ts 29RangeError sx2sy3 objectrt1 1fn2fnc3ff4 o10.20.|20.40.| bottom0 / iaf1f2,103,5,2", "row4": "3bbnt|3bb 291 12r0nt;4;8nt; iaf1c 4 E:7 5:10 5,8,init,vo 5,8,init,ts 5,init,ts 30RangeError sx2sy4 object4,nt,z 1fn2fn3fnf4 o10.20.|20.40.| bottom0 / iaf1cf2,104,6,2", "row5": "4b255|4b25 262 15r0nt;5;10c; iaf1ret 5 E:8 116:11 6,10,init,vo 6,10,init,ts 6,init,ts 31RangeError sx2sy5 objectrt1 1fn2fn3fnf4 o10.20.|20.40.| bottom0 / iaf1f2,105,7,2", "row6": "3bbnt|3bb 285 18r0nt;6;12nt; iaf1c 6 E:9 7:10 7,12,init,vo 7,12,init,ts 7,init,ts 32RangeError sx2sy6 object6,nt,z 1fn2fn3fnf4 o10.20.|20.40.| bottom0 / iaf1cf2,106,8,2", "row7": "4b277|4b27 264 21r0nt;7;14c; iaf1ret 7 E:10 18:111 8,14,init,vo 8,14,init,ts 8,init,ts 33RangeError sx2sy7 objectrt1 1fn2fn3fnf4 o10.20.|20.40.| bottom0 / iaf1f2,107,9,2", "row15": "4b21515|4b215 272 45r0nt;15;30c; iaf1ret 15 E:18 26:111 16,30,init,vo 16,30,init,ts 16,init,ts 41RangeError sx2sy15 objectrt1 1fn2fn3fnf4 o10.20.|20.40.| bottom0 / iaf1f2,115,17,2", "row23": "4b22323|4b223 280 69r0nt;23;46c; iaf1ret 23 E:26 34:111 24,46,init,vo 24,46,init,ts 24,init,ts 49RangeError sx2sy23 objectrt1 1fn2fn3fnf4 o10.20.|20.40.| bottom0 / iaf1f2,123,25,2", "row31": "4b23131|4b231 288 93r0nt;31;62c; iaf1ret 31 E:34 42:111 32,62,init,vo 32,62,init,ts 32,init,ts 57RangeError sx2sy31 objectrt1 1fn2fn3fnf4 o10.20.|20.40.| bottom0 / iaf1f2,131,33,2", "row39": "4b23939|4b239 296 117r0nt;39;78c; iaf1ret 39 E:42 50:111 40,78,init,vo 40,78,init,ts 40,init,ts 65RangeError sx2sy39 objectrt1 1fn2fn3fnf4 o10.20.|20.40.| bottom0 / iaf1f2,139,41,2", "row47": "4b24747|4b247 304 141r0nt;47;94c; iaf1ret 47 E:50 58:111 48,94,init,vo 48,94,init,ts 48,init,ts 73RangeError sx2sy47 objectrt1 1fn2fn3fnf4 o10.20.|20.40.| bottom0 / iaf1f2,147,49,2", "row55": "4b25555|4b255 312 165r0nt;55;110c; iaf1ret 55 E:58 66:111 56,110,init,vo 56,110,init,ts 56,init,ts 81RangeError sx2sy55 objectrt1 1fn2fn3fnf4 o10.20.|20.40.| bottom0 / iaf1f2,155,57,2", "row63": "4b26363|4b263 320 189r0nt;63;126c; iaf1ret 63 E:66 74:111 64,126,init,vo 64,126,init,ts 64,init,ts 89RangeError sx2sy63 objectrt1 1fn2fn3fnf4 o10.20.|20.40.| bottom0 / iaf1f2,163,65,2", "positions0": "null is not an object (evaluating 'null.foo') @45:26", "positions1": "undefinedFunction is not defined @46:39", "positions2": "t2 @10:45", "positions3": "undefined is not an object (evaluating 'o.x.y') @48:41", "positions4": "undefined is not a constructor (evaluating 'new (void 0)()') @49:38", "positions5": "null is not an object (evaluating '[a, b]') @50:22", "positions6": "Cannot destructure property 'a' from null or undefined value @51:40", "positions7": "number is not iterable @52:35", "positions8": "undefined is not an object (evaluating 'b.c.d') @53:29", "positions9": "ts @11:81", "positions10": "v is not an Object. (evaluating 'v in v') @55:32", "positions11": "direct @56:42", "positions12": "Cannot access 'zz' before initialization. @57:36", "positions13": "v is not a function. (In 'v()', 'v' is 13) @58:24", "positions14": "Cannot convert a symbol to a string @59:27", "positions15": "undefined is not an object (evaluating 'a.b.c') @60:26", "positions16": "arr[Symbol.iterator].call(bad) is not a function. (In 'arr[Symbol.iterator].call(bad)()', 'arr[Symbol.iterator].call(bad)' is an instance of Array Iterator) @61:53", "positions17": "none 181818,18,nt", "positions0r1": "null is not an object (evaluating 'null.foo') @45:26", "positions1r1": "undefinedFunction is not defined @46:39", "positions2r1": "t2 @10:45", "positions3r1": "undefined is not an object (evaluating 'o.x.y') @48:41", "positions4r1": "undefined is not a constructor (evaluating 'new (void 0)()') @49:38", "positions5r1": "null is not an object (evaluating '[a, b]') @50:22", "positions6r1": "Cannot destructure property 'a' from null or undefined value @51:40", "positions7r1": "number is not iterable @52:35", "positions8r1": "undefined is not an object (evaluating 'b.c.d') @53:29", "positions9r1": "ts @11:81", "positions10r1": "v is not an Object. (evaluating 'v in v') @55:32", "positions11r1": "direct @56:42", "positions12r1": "Cannot access 'zz' before initialization. @57:36", "positions13r1": "v is not a function. (In 'v()', 'v' is 13) @58:24", "positions14r1": "Cannot convert a symbol to a string @59:27", "positions15r1": "undefined is not an object (evaluating 'a.b.c') @60:26", "positions16r1": "arr[Symbol.iterator].call(bad) is not a function. (In 'arr[Symbol.iterator].call(bad)()', 'arr[Symbol.iterator].call(bad)' is an instance of Array Iterator) @61:53", "positions17r1": "none 181818,18,nt", "positions0r2": "null is not an object (evaluating 'null.foo') @45:26", "positions1r2": "undefinedFunction is not defined @46:39", "positions2r2": "t2 @10:45", "positions3r2": "undefined is not an object (evaluating 'o.x.y') @48:41", "positions4r2": "undefined is not a constructor (evaluating 'new (void 0)()') @49:38", "positions5r2": "null is not an object (evaluating '[a, b]') @50:22", "positions6r2": "Cannot destructure property 'a' from null or undefined value @51:40", "positions7r2": "number is not iterable @52:35", "positions8r2": "undefined is not an object (evaluating 'b.c.d') @53:29", "positions9r2": "ts @11:81", "positions10r2": "v is not an Object. (evaluating 'v in v') @55:32", "positions11r2": "direct @56:42", "positions12r2": "Cannot access 'zz' before initialization. @57:36", "positions13r2": "v is not a function. (In 'v()', 'v' is 13) @58:24", "positions14r2": "Cannot convert a symbol to a string @59:27", "positions15r2": "undefined is not an object (evaluating 'a.b.c') @60:26", "positions16r2": "arr[Symbol.iterator].call(bad) is not a function. (In 'arr[Symbol.iterator].call(bad)()', 'arr[Symbol.iterator].call(bad)' is an instance of Array Iterator) @61:53", "positions17r2": "none 181818,18,nt", "deepStack": "42", "toStringUnchanged": "861:ly { x += \"|\"; } } return x; }"}; let __checked = 0; function check(name, value) { if (!(name in __expect)) throw new Error("no expectation recorded for " + name); if (String(value) !== __expect[name]) throw new Error(name + ": expected " + JSON.stringify(__expect[name]) + " but got " + JSON.stringify(String(value))); __checked++; } const __expectedChecks = 71;

function run(f, ...a) { try { return String(f(...a)); } catch (e) { return "E:" + (e && e.message || e); } }
function thrower(x) { if (x) throw new Error("t" + x); return "nt"; }
const bad = { valueOf() { throw new Error("vo"); }, toString() { throw new Error("ts"); } };
const og = { get x() { throw new RangeError("gx"); }, set y(v) { throw new TypeError("sy" + v); } };

function t1(v) { let a = 1, b = "b", c = v; try { a = 2; c = thrower(v & 1); a = 3; b = "bb"; } catch (e) { b = b + a + c; a = 4; } finally { c = c + "|" + a + b; } return a + b + c; }
function t2(v) { let a = v; l: try { a += 1; if (v & 1) break l; a += 2; try { a += 4; thrower(v & 2); a += 8; } finally { a += 16; if (v & 4) break l; a += 32; } a += 64; } catch (e) { a += 128; } finally { a += 256; } return a; }
function t3(v) { t3.log = ""; for (let i = 0; i < 4; i++) { let x = i * v; try { if (i == 1) continue; if (i == 3) return x + "r" + t3.log; x += thrower(i == 2 && v & 1); } catch (e) { x += "c"; continue; } finally { t3.log += x + ";"; } } return "end"; }
function t4(v) { let r = "i"; try { try { r += "a"; thrower(1); } finally { r += "f1"; if (v & 1) return r + "ret"; } } catch (e) { r += "c"; return r; } finally { r += "f2"; t4.last = r; } }
function t5(v) { let x = v; try { return x; } finally { x = x + 100; t5.side = x; } }
function t6(v) { let x = v; try { throw x + 1; } catch (y) { x = y + 1; throw x + 1; } finally { t6.side = x; } }
function t7(v) { let a = v, b = 0; try { try { a++; thrower(v & 1); } catch (e) { b++; a += 10; thrower(v & 2); a += 100; } finally { b += 10; } } catch (e2) { b += 100; } return a + ":" + b; }
function t8(v) { let a = v, b = v * 2, c = "init"; try { a = a + 1; b = b + bad; c = "not"; } catch (e) { return [a, b, c, e.message].join(); } return "no"; }
function t9(v) { let a = v, b = v * 2, c = "init"; try { a = a + 1; c = `${a}${bad}`; b = 0; } catch (e) { return [a, b, c, e.message].join(); } return "no"; }
function t10(v) { let a = [v], c = "init"; try { a[0]++; c = String(bad); } catch (e) { return [a, c, e.message].join(); } finally { a.push(c); t10.side = a.length; } }
function t11(v) { let a = v | 0, b; try { a += 2; b = og.x; a += 4; } catch (e) { a += 8; b = e.name; } finally { a += 16; } return a + b; }
function t12(v) { let s = "s", n = 0; try { n = 1; og.y = (n = 2, s += "x", v); n = 3; } catch (e) { return s + n + e.message; } }
function t13(v) { let k = { v }; let r = "r"; try { r = [k.v, thrower(v & 1), (k = null, "z")].join(); } catch (e) { return typeof k + r + e.message; } return typeof k + r; }
function t14(v) { let i = 0, acc = ""; while (true) { try { i++; if (i > 3) break; if (i == v) throw i; acc += i; } catch (e) { acc += "c" + e; continue; } finally { acc += "f"; } acc += "n"; } return acc + i; }
function t15(v) { let x = "o"; outer: for (const a of [1, 2]) { try { for (const b of [10, 20]) { try { if (a * b == v) break outer; if (a + b == v) continue outer; x += a * b; } finally { x += "."; } } } finally { x += "|"; } } return x; }
function t16(v) { let depth = 0; function rec(n) { depth++; try { if (n == 0) throw new Error("bottom"); return rec(n - 1) + 1; } finally { depth--; } } try { return rec(v & 7); } catch (e) { return e.message + depth; } }

const fns = [t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15, t16];
for (let i = 0; i < 64; i++) {
    const row = fns.map(f => run(f, i)).join(" ") + " / " + [t4.last, t5.side, t6.side, t10.side].join();
    if (i < 8 || i % 8 == 7)
        check("row" + i, row);
}

// Source positions: line:column of throw sites and the source text quoted in messages must be unchanged.
function where(e) { const top = String(e.stack).split("\n")[0]; return String(e.message) + " @" + top.slice(top.lastIndexOf(":") - 6).replace(/^[^:]*:/, ""); }
function positions(v) {
    let a = v + 1;
    let b = a;
    const arr = [a, b, thrower(0)];
    try {
        if (v === 0) null.foo;
        if (v === 1) undefinedFunction();
        if (v === 2) b = thrower(2);
        if (v === 3) { const o = {}; o.x.y.z = 1; }
        if (v === 4) a = new (void 0)();
        if (v === 5) [a, b] = null;
        if (v === 6) ({ a } = undefined);
        if (v === 7) for (const q of 5) { }
        if (v === 8) a = b.c.d;
        if (v === 9) `${a}${bad}`;
        if (v === 10) a = v in v;
        if (v === 11) throw new TypeError("direct");
        if (v === 12) { let zz = zz; }
        if (v === 13) v();
        if (v === 14) a = Symbol() + "";
        if (v === 15) a.b.c();
        if (v === 16) arr[Symbol.iterator].call(bad)();
    } catch (e) {
        return where(e);
    }
    return "none " + a + b + arr;
}
for (let rep = 0; rep < 3; rep++)
    for (let i = 0; i < 18; i++)
        check("positions" + i + (rep ? "r" + rep : ""), positions(i));

function deep(n) { if (n == 0) return new Error("d").stack.split("\n").length; const keep = { n }; const r = deep(n - 1); return keep.n === n ? r : -1; }
check("deepStack", deep(40));
check("toStringUnchanged", positions.toString().length + ":" + t15.toString().slice(-30));

if (__checked !== __expectedChecks)
    throw new Error("expected " + __expectedChecks + " checks, ran " + __checked);
