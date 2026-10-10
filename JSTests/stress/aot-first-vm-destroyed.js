//@ skip if $architecture != "arm64"
//@ requireOptions("--compileMainScriptAheadOfTime=1", "--destroy-another-vm-first")
//@ defaultRun
//@ run("gc-stress", "--slowPathAllocsBetweenGCs=20")

class Point {
    constructor(x) { this.x = x; }
    get double() { return this.x * 2; }
}
function work(n) {
    let out = [];
    for (let i = 0; i < n; i++)
        out.push([new Point(i).double, `t${i}`, /(\d)$/.exec("a" + i)[1], 12345678901234567890n % BigInt(i + 2), Symbol.for("shared").description, ["a", "b"].includes("b")].join());
    return out.at(-1);
}

const expected = "198,t99,9,0,shared,true";
if (!isAOTCompiled(work) || !isAOTCompiled(Point))
    throw new Error("not compiled");
if (work(100) !== expected)
    throw new Error(`got ${work(100)}`);
fullGC();
if (work(100) !== expected)
    throw new Error(`after a collection, got ${work(100)}`);
