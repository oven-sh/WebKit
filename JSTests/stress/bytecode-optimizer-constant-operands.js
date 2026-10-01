//@ runDefault("--useBytecodeOptimizer=1")
//@ runDefault("--useBytecodeOptimizer=1", "--useConcurrentJIT=0", "--jitPolicyScale=0")
// Copy propagation forwards constant registers into the operands of later instructions. Every consumer must
// accept a constant there: LLInt's op_jneq_ptr / op_jeq_ptr read |value| with a raw frame load, so a local
// `Array`/`Object`/`eval` binding that holds a constant must not be propagated into the special-pointer check
// (in the narrow encoding constant #k aliases call-frame slot 16 + k, i.e. argument #(11 + k) of the caller).
const __expect = {"shadowArray": "TypeError", "shadowArrayNumber": "TypeError", "shadowObject": "TypeError", "shadowEval": "TypeError", "constantConsumers": "sw-s,sw-7,imm-default,big-truthy,true,false,object,bigint,3,t,str7,37,sloppy-put-ok,TypeError,TypeError,caught:str,3,str|7|null,M,false,true,false,,false,-7,NaN,-8,49,true,true,-1,7,s,t,r ## sw-s,sw-7,imm-default,big-truthy,true,false,object,bigint,3,t,str7,37,sloppy-put-ok,TypeError,TypeError,caught:str,3,str|7|null,7,false,true,false,,false,-7,NaN,-8,49,true,true,-1,M,s,t,r ## sw-s,sw-7,imm-default,big-truthy,true,false,object,bigint,3,t,str7,M,sloppy-put-ok,TypeError,TypeError,caught:str,3,str|7|null,7,false,true,false,,false,-7,NaN,-8,49,true,true,-1,37,s,t,r ## sw-s,sw-7,imm-default,big-truthy,true,false,object,bigint,3,t,str7,37,sloppy-put-ok,TypeError,TypeError,caught:str,3,str|7|null,7,false,true,false,,false,-7,NaN,-8,M,true,true,-1,49,s,t,r"}; let __checked = 0; function check(name, value) { if (!(name in __expect)) throw new Error("no expectation recorded for " + name); if (String(value) !== __expect[name]) throw new Error(name + ": expected " + JSON.stringify(__expect[name]) + " but got " + JSON.stringify(String(value))); __checked++; } const __expectedChecks = 5;

function shadowArray(n) {
    const Array = "notArray";
    try { return "constructed " + typeof new Array(n); } catch (e) { return e.constructor.name; }
}
function shadowArrayNumber(n) {
    let Array = 42;
    try { return "constructed " + typeof new Array(n, n); } catch (e) { return e.constructor.name; }
}
function shadowObject() {
    const Object = null;
    try { return "constructed " + typeof new Object(); } catch (e) { return e.constructor.name; }
}
function shadowEval(s) {
    const eval = undefined;
    try { return "evaluated " + eval(s); } catch (e) { return e.constructor.name; }
}
const shadowResults = { shadowArray: [], shadowArrayNumber: [], shadowObject: [], shadowEval: [] };
for (let i = 0; i < 100; i++) {
    // Plant the real constructors in the argument slots (#7..#16) that alias constant registers #0..#9 when an
    // operand holding constant #k is (mis)read as frame slot 16 + k.
    shadowResults.shadowArray.push(shadowArray(3, 1, 2, 3, 4, 5, 6, Array, Array, Array, Array, Array, Array, Array, Array, Array, Array));
    shadowResults.shadowArrayNumber.push(shadowArrayNumber(3, 1, 2, 3, 4, 5, 6, Array, Array, Array, Array, Array, Array, Array, Array, Array, Array));
    shadowResults.shadowObject.push(shadowObject(0, 1, 2, 3, 4, 5, 6, Object, Object, Object, Object, Object, Object, Object, Object, Object, Object));
    shadowResults.shadowEval.push(shadowEval("1+1", 1, 2, 3, 4, 5, 6, eval, eval, eval, eval, eval, eval, eval, eval, eval, eval));
}
for (const name in shadowResults)
    check(name, [...new Set(shadowResults[name])]);

// Other consumers of propagated constants (all must behave exactly like a register operand).
function constantConsumers(i) {
    const s = "str", big = 123456789012345678901234567890n, nul = null, num = 7, sym = "sym";
    const r = [];
    switch (s) { case "str": r.push("sw-s"); break; default: r.push("sw-d"); }
    switch (num) { case 7: r.push("sw-7"); break; case 8: r.push("sw-8"); break; default: r.push("sw-dn"); }
    switch (s) { case 1: r.push("imm-on-string"); break; default: r.push("imm-default"); }
    if (big) r.push("big-truthy"); else r.push("big-falsy");
    r.push(nul == undefined, nul === undefined, typeof nul, typeof big, s.length, s[1], s + num, num + big.toString().length);
    try { s.x = 1; r.push("sloppy-put-ok"); } catch (e) { r.push(e.constructor.name); }
    try { r.push("x" in s); } catch (e) { r.push(e.constructor.name); }
    try { r.push(nul.p); } catch (e) { r.push(e.constructor.name); }
    try { throw s; } catch (e) { r.push("caught:" + e); }
    r.push([s, num, nul].length, `${s}|${num}|${nul}`, { [s]: num }[sym.slice(0, 1) + "tr"], s instanceof Object, Object(s) instanceof String);
    r.push(delete s.length, void num, !s, -num, +s, ~num, num ** 2, s < "t", num <= 7, [1, 2, 3].indexOf(num), Math.max(num, i));
    for (const c of s) r.push(c);
    for (const k in nul) r.push("unreachable");
    return r.join(",");
}
const consumerResults = new Set;
for (let i = 0; i < 60; i++)
    consumerResults.add(constantConsumers(i).replace("," + Math.max(7, i) + ",", ",M,"));
check("constantConsumers", [...consumerResults].join(" ## "));

if (__checked !== __expectedChecks)
    throw new Error("expected " + __expectedChecks + " checks, ran " + __checked);
