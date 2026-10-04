//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlineGuessedPlacesEverywhere=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlineGuessedPlacesEverywhere=1", "--validateGraphAtEachPhase=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlineGuessedPlacesEverywhere=1", "--useAOTInlining=0", "--validateGraphAtEachPhase=1", "--validateAOTInferredTypes=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlineGuessedPlacesEverywhere=1", "--useAOTGuessedPlaces=0")
//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault

function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
const remarksOf = f => typeof aotRemarks === "function" && aotRemarks(f.name) || null;
const options = typeof jscOptions === "function" ? jscOptions() : { };
const isInline = !!remarksOf(check) && !!options.useAOTInlineGuessedPlacesEverywhere && !!options.useAOTGuessedPlaces && !!options.useAOTDataStubs;
function shares(f, expected) {
    if (isInline)
        check(remarksOf(f).includes("reuses-structure-of-base"), expected, f.name + " reuses the structure of its base");
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

function storesTwice(x, a, b) {
    x.tag = a;
    x.key = b;
}
function storesThenReads(x, a) {
    x.child = a;
    return x.flags;
}
function readsThenStores(x, a) {
    let before = x.tag;
    x.key = a;
    return before;
}
function storesAroundCall(x, a, b) {
    x.tag = a;
    keep(x);
    x.key = b;
}
function storesToTwo(x, y, a, b, c, d) {
    x.tag = a;
    y.tag = b;
    x.key = c;
    y.key = d;
}
for (let f of [storesTwice, storesThenReads, readsThenStores, storesAroundCall, storesToTwo])
    noInline(f);

for (let i = 0; i < 200; ++i) {
    let x = makeItem(i), y = makeItem(-i);
    storesTwice(x, i, i + 1);
    check(x.tag, i, "storesTwice: tag");
    check(x.key, i + 1, "storesTwice: key");
    check(storesThenReads(x, i + 2), "flags" + i, "storesThenReads: flags");
    check(x.child, i + 2, "storesThenReads: child");
    check(readsThenStores(x, i + 3), i, "readsThenStores: tag");
    check(x.key, i + 3, "readsThenStores: key");
    storesAroundCall(x, i + 4, i + 5);
    check(x.tag + x.key, 2 * i + 9, "storesAroundCall");
    storesToTwo(x, y, 1, 2, 3, 4);
    check([x.tag, y.tag, x.key, y.key].join(), "1,2,3,4", "storesToTwo");
    storesToTwo(x, x, 1, 2, 3, 4);
    check([x.tag, x.key].join(), "2,4", "storesToTwo on one object");
}
shares(storesTwice, true);
shares(storesThenReads, true);
shares(readsThenStores, true);
shares(storesToTwo, true);
shares(storesAroundCall, false);

function movesKeyWhenTagIsStored() {
    let o = build(["filler", 0], ["key", "old"], ["child", 0], ["flags", "f"]);
    let log = [];
    Object.defineProperty(o, "tag", {
        configurable: true,
        set(value) {
            log.push("set " + value);
            delete o.key;
            delete o.filler;
            o.other = 1;
            o.key = "moved";
        },
    });
    return { o, log };
}
for (let i = 0; i < 50; ++i) {
    let { o, log } = movesKeyWhenTagIsStored();
    storesTwice(o, "a" + i, "b" + i);
    check(log.join(), "set a" + i, "the setter runs once");
    check(o.key, "b" + i, "the store behind a setter that moves the property");
    check(o.other, 1, "the property that took the slot is left alone");
    check(o.flags, "f", "the neighbour is left alone");
}

function becomesItemLikeWhenChildIsStored(i) {
    let o = build(["tag", 0], ["key", 1]);
    Object.defineProperty(o, "child", {
        configurable: true,
        set(value) {
            delete o.child;
            o.child = value;
            o.flags = "late" + i;
        },
    });
    return o;
}
for (let i = 0; i < 50; ++i) {
    let o = becomesItemLikeWhenChildIsStored(i);
    check(storesThenReads(o, i), "late" + i, "the read behind a setter that adds the property");
    check(o.child, i, "the setter's own store");
}

let frozen = Object.freeze(makeItem("frozen"));
storesTwice(frozen, 1, 2);
check(frozen.tag + frozen.key, "tagfrozenkeyfrozen", "a frozen object is left alone");
storesTwice(5, 1, 2);
storesTwice("text", 1, 2);
for (let nothing of [null, undefined]) {
    let thrown = "nothing";
    try {
        storesTwice(nothing, 1, 2);
    } catch (error) {
        thrown = error.constructor.name;
    }
    check(thrown, "TypeError", "a store to " + nothing);
}
let proxyLog = [];
let proxy = new Proxy(makeItem("target"), { set(target, name, value) { proxyLog.push(String(name) + "=" + value); return true; } });
storesTwice(proxy, 1, 2);
check(proxyLog.join(), "tag=1,key=2", "a Proxy sees both stores");
