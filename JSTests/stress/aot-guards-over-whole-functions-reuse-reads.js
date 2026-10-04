//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--validateGraphAtEachPhase=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTInlining=0", "--validateGraphAtEachPhase=1", "--validateAOTInferredTypes=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTOperationCounters=1")
//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault

function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
const remarksOf = f => typeof aotRemarks === "function" && aotRemarks(f.name) || null;
const options = typeof jscOptions === "function" ? jscOptions() : { };
const isOn = !!remarksOf(check) && !!options.useAOTGuardsOverWholeFunctions && !!options.useAOTGuessedPlaces && !!options.useAOTDataStubs;
function repeats(f, name, checksNothing, reuses) {
    if (!isOn)
        return;
    check(remarksOf(f).includes("guards-over-whole-function"), true, f.name + " has guards over the whole function");
    check(remarksOf(f).includes("guard-checks-nothing:" + name), checksNothing, f.name + " has a guard on " + name + " that checks nothing");
    check(remarksOf(f).includes("reuses-property-read:" + name), reuses, f.name + " reuses its read of " + name);
}

const kept = [];
function keep(o) { kept.push(o); return o; }
noInline(keep);
function build(...pairs) {
    let o = {};
    for (let [name, value] of pairs)
        o[name] = value;
    return keep(o);
}
function Item(which) {
    this.tag = "tag" + which;
    this.key = "key" + which;
    this.child = "child" + which;
    this.flags = "flags" + which;
}
const makeItem = which => keep(new Item(which));

function readsTagTwice(x, y, z) {
    let first = x.tag;
    let second = y.key;
    let third = z.child;
    let again = x.tag;
    return [first, second, third, again];
}
function readsTagAroundArithmetic(x, y, z, number) {
    let first = x.tag;
    let second = y.key;
    let third = z.child;
    let sum = number + 1;
    let again = x.tag;
    return [first, second, third, sum, again];
}
function readsTagAroundStoreOfTag(x, y, z, other) {
    let first = x.tag;
    let second = y.key;
    other.tag = z.child;
    let again = x.tag;
    return [first, second, again];
}
function readsTagAroundStoreOfKey(x, y, z, other) {
    let first = x.tag;
    let second = y.child;
    other.key = z.child;
    let again = x.tag;
    return [first, second, again];
}
function readsTagAroundCall(x, y, z, callee) {
    let first = x.tag;
    let second = y.key;
    let third = z.child;
    callee(x);
    let again = x.tag;
    return [first, second, third, again];
}
for (let f of [readsTagTwice, readsTagAroundArithmetic, readsTagAroundStoreOfTag, readsTagAroundStoreOfKey, readsTagAroundCall])
    noInline(f);

for (let i = 0; i < 200; ++i) {
    let x = makeItem("x" + i), y = makeItem("y" + i), z = makeItem("z" + i), other = makeItem("o" + i);
    check(readsTagTwice(x, y, z).join(), `tagx${i},keyy${i},childz${i},tagx${i}`, "readsTagTwice");
    check(readsTagAroundArithmetic(x, y, z, i).join(), `tagx${i},keyy${i},childz${i},${i + 1},tagx${i}`, "readsTagAroundArithmetic");
    check(readsTagAroundStoreOfTag(x, y, z, other).join(), `tagx${i},keyy${i},tagx${i}`, "readsTagAroundStoreOfTag, another object");
    check(other.tag, "childz" + i, "the store of tag");
    check(readsTagAroundStoreOfTag(x, y, z, x).join(), `tagx${i},keyy${i},childz${i}`, "readsTagAroundStoreOfTag, the same object");
    x.tag = "tagx" + i;
    check(readsTagAroundStoreOfKey(x, y, z, x).join(), `tagx${i},childy${i},tagx${i}`, "readsTagAroundStoreOfKey");
    check(x.key, "childz" + i, "the store of key");
    check(readsTagAroundCall(x, y, z, o => { o.tag = "called" + i; }).join(), `tagx${i},keyy${i},childz${i},called${i}`, "readsTagAroundCall");
}
repeats(readsTagTwice, "tag", true, false);
repeats(readsTagAroundStoreOfKey, "tag", true, false);
repeats(readsTagAroundStoreOfTag, "tag", true, false);
repeats(readsTagAroundArithmetic, "tag", false, true);
repeats(readsTagAroundCall, "tag", false, false);

for (let i = 0; i < 50; ++i) {
    let x = makeItem("x"), y = makeItem("y"), z = makeItem("z");
    let changes = { valueOf() { x.tag = "changed" + i; return 10; } };
    check(readsTagAroundArithmetic(x, y, z, changes).join(), `tagx,keyy,childz,11,changed${i}`, "an operand that stores the name");
    x = makeItem("x");
    let moves = { valueOf() { delete x.tag; x.late = 1; x.tag = "moved" + i; return 20; } };
    check(readsTagAroundArithmetic(x, y, z, moves).join(), `tagx,keyy,childz,21,moved${i}`, "an operand that moves the name");
    x = makeItem("x");
    let log = [];
    let redefines = { valueOf() { Object.defineProperty(x, "tag", { get() { log.push("get"); return "got" + i; } }); return 30; } };
    check(readsTagAroundArithmetic(x, y, z, redefines).join(), `tagx,keyy,childz,31,got${i}`, "an operand that makes the name an accessor");
    check(log.join(), "get", "the accessor runs once");
}

let log = [];
let withGetter = keep(Object.defineProperty(build(["key", 1], ["child", 2]), "tag", { get() { log.push("get"); return "got"; } }));
check(readsTagTwice(withGetter, makeItem("y"), makeItem("z")).join(), "got,keyy,childz,got", "an accessor from the start");
check(log.join(), "get,get", "it runs at both reads");
let reads = [];
let proxy = new Proxy(makeItem("p"), { get(target, name) { reads.push(String(name)); return target[name]; } });
check(readsTagTwice(proxy, makeItem("y"), makeItem("z")).join(), "tagp,keyy,childz,tagp", "a Proxy");
check(reads.join(), "tag,tag", "a Proxy sees both reads");
