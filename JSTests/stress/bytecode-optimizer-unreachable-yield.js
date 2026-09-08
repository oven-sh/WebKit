//@ runDefault("--useBytecodeOptimizer=1")
//@ runDefault("--useBytecodeOptimizer=1", "--useConcurrentJIT=0", "--jitPolicyScale=0")
//@ runDefault("--useBytecodeOptimizer=1", "--useConcurrentJIT=0", "--jitPolicyScale=0", "--useFTLJIT=1", "--forceOSRExitToLLInt=1")
// Unreachable-code elimination deletes op_yield instructions the generator emitted under constant-false
// conditions. Generatorification must still produce a well-formed dispatch table for the surviving yields:
// a hole must not turn into a jump to offset 0 (op_enter), which gives the DFG root block a predecessor
// (hang / validation failure once the generator body tiers up). Bodies are driven hot on purpose.
const __expect = {"holeFirst": "450075000", "generators0": "0/1/R:0,0,b 0/1/R:done0 0/R:end -1/caught neg/R:end", "generators1": "1/3/R:1,2,b 1/2/R:done1 1/R:end -2/caught neg/R:end", "generators2998": "2998/5997/R:2998,5996,b 2998/2999/R:done2998 2998/R:end -2999/caught neg/R:end", "generators2999": "2999/5999/R:2999,5998,b 2999/3000/R:done2999 2999/R:end -3000/caught neg/R:end", "finallyCount": "6000", "returnThroughFinally": "{\"value\":\"r\",\"done\":true}6001", "throwIntoGenerator": "x{\"done\":true}", "holeAsync": "25000000", "holeAsyncGen": "600:0,1,2,0,1,2,0"}; let __checked = 0; function check(name, value) { if (!(name in __expect)) throw new Error("no expectation recorded for " + name); if (String(value) !== __expect[name]) throw new Error(name + ": expected " + JSON.stringify(__expect[name]) + " but got " + JSON.stringify(String(value))); __checked++; } const __expectedChecks = 10;

function* holeFirst(x) {
    if (false) yield -1;
    const sent = yield x;
    return sent + x;
}
function* holeMiddle(x) {
    let a = yield x;
    if (0) { a = yield "dead"; yield* [1, 2]; }
    const b = yield a + 1;
    return [x, a, b].join();
}
function* holeLast(x) {
    yield x; yield x + 1;
    while ("") yield "dead";
    return "done" + x;
}
function* holeInTry(x) {
    try {
        if (null) yield 0;
        yield x;
        if (x < 0) throw new Error("neg");
    } catch (e) {
        if (void 0) yield "dead";
        yield "caught " + e.message;
    } finally {
        if (!1) yield "dead";
        holeInTry.finallyCount = (holeInTry.finallyCount | 0) + 1;
    }
    return "end";
}
async function holeAsync(x) {
    if (false) await null;
    const a = await x;
    if (0) return await a;
    const b = await (a + 1);
    return a + b;
}
async function* holeAsyncGen(n) {
    if (false) { yield -1; await null; }
    for (let i = 0; i < n; i++) {
        if ("") yield "dead";
        yield await Promise.resolve(i);
    }
}

function drive(g, ...sends) {
    const out = [];
    let r, i = 0;
    while (!(r = g.next(sends[i++])).done)
        out.push(r.value);
    out.push("R:" + r.value);
    return out.join("/");
}

let acc = 0;
for (let i = 0; i < 30000; i++) {
    const it = holeFirst(i);
    it.next();
    acc += it.next(3).value;
}
check("holeFirst", acc);

let s = "";
for (let i = 0; i < 3000; i++) {
    s = drive(holeMiddle(i), undefined, i * 2, "b") + " " + drive(holeLast(i)) + " " + drive(holeInTry(i)) + " " + drive(holeInTry(-i - 1));
    if (i < 2 || i > 2997)
        check("generators" + i, s);
}
check("finallyCount", holeInTry.finallyCount);
{
    const it = holeInTry(5); it.next(); // suspended at `yield x` inside the try: return() must run the finally
    check("returnThroughFinally", JSON.stringify(it.return("r")) + holeInTry.finallyCount);
    const it2 = holeMiddle(1); it2.next();
    let thrown; try { it2.throw(new Error("x")); } catch (e) { thrown = e.message; }
    check("throwIntoGenerator", thrown + JSON.stringify(it2.next()));
}

let asyncError = null, asyncSum = 0, asyncGen = [];
(async () => {
    for (let i = 0; i < 5000; i++)
        asyncSum += await holeAsync(i);
    for (let round = 0; round < 200; round++)
        for await (const v of holeAsyncGen(3)) asyncGen.push(v);
})().catch(e => { asyncError = e; });
drainMicrotasks();
if (asyncError)
    throw asyncError;
check("holeAsync", asyncSum);
check("holeAsyncGen", asyncGen.length + ":" + asyncGen.slice(0, 7));

if (__checked !== __expectedChecks)
    throw new Error("expected " + __expectedChecks + " checks, ran " + __checked);
