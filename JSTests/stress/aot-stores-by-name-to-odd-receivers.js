//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTLoopSplitting=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--useAOTOperationCounters=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--validateGraphAtEachPhase=1", "--validateAOTInferredTypes=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--validateGraphAtEachPhase=1", "--validateAOTInferredTypes=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTDataStubs=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--verifyGC=1", "--scribbleFreeCells=1", "--useZombieMode=1", "--slowPathAllocsBetweenGCs=50")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsAtEveryGuessedPlaceForTesting=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsAtEveryGuessedPlaceForTesting=1", "--useAOTInlining=0", "--validateGraphAtEachPhase=1", "--validateAOTInferredTypes=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsAtEveryGuessedPlaceForTesting=1", "--useAOTInlining=0", "--failEveryNthAOTGuardForTesting=2")
//@ runDefault

function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function throwsTypeError(f, what) {
    try {
        f();
    } catch (e) {
        check(e instanceof TypeError, true, what);
        return;
    }
    throw new Error(what + ": nothing was thrown");
}

const kept = [];
function keep(o) { kept.push(o); return o; }

function Item(tag, key) {
    this.tag = tag;
    this.key = key;
    this.child = null;
    this.sibling = null;
    this.flags = 0;
}
function makeItem(tag, key) { return keep(new Item(tag, key)); }
function build(...pairs) {
    let o = {};
    for (let [name, value] of pairs)
        o[name] = value;
    return keep(o);
}
function withSetter(name, set) { return Object.defineProperty({ }, name, { set, enumerable: true, configurable: true }); }
function describe(o) { return Object.keys(o).map(name => name + "=" + String(o[name])).join(); }

function setFlags(item, value) { item.flags = value; }
function setFlagsStrictly(item, value) { "use strict"; item.flags = value; }
function setChild(item, child) { item.child = child; }
function setTagAndSibling(item, tag, sibling) { item.tag = tag; item.sibling = sibling; }
function setSelf(item) { item.child = item; }
function setFlagsOfAll(items, value) {
    for (let i = 0; i < items.length; ++i)
        items[i].flags = value + i;
}
function setChildOfAll(items, make) {
    for (let i = 0; i < items.length; ++i)
        items[i].child = make(i);
}
function bump(item) { item.flags = item.flags + 1; return item.flags; }
function setNeverBorn(item, value) { item.neverBornAnywhere = value; }
function bornHere(value) { let o = { first: 1, second: 2 }; o.second = value; return keep(o); }
function defines(value) { return keep({ first: value, second: value }); }

function Named(toString) {
    this.toString = toString;
    this.other = 1;
}
function setToString(named, toString) { named.toString = toString; }
class Born {
    constructor(first, second) {
        this.bornFirst = first;
        this.bornSecond = second;
        this.bornFirst = second;
    }
}
function setBornSecond(born, value) { born.bornSecond = value; }

for (let f of [Item, makeItem, setFlags, setFlagsStrictly, setChild, setTagAndSibling, setSelf, setFlagsOfAll, setChildOfAll, bump, setNeverBorn, bornHere, defines, Named, setToString, Born, setBornSecond])
    globalThis[f.name + "Kept"] = f;


for (let round = 0; round < 40; ++round) {
    let item = makeItem(1, "k");
    setFlags(item, round);
    check(describe(item), "tag=1,key=k,child=null,sibling=null,flags=" + round, "an object as it was born");
    setFlagsStrictly(item, -round);
    check(item.flags, -round, "the same, strictly");
    setTagAndSibling(item, "t", 2.5);
    check(describe(item), "tag=t,key=k,child=null,sibling=2.5,flags=" + -round, "two names of one object");
    check(bump(item), 1 - round, "read, then stored, then read");
    setSelf(item);
    check(item.child, item, "an object stored into itself");

    let items = [makeItem(1, "a"), makeItem(2, "b"), makeItem(3, "c")];
    setFlagsOfAll(items, 10);
    check(items.map(each => each.flags).join(), "10,11,12", "in a loop");
    setChildOfAll(items, i => ({ number: i, round }));
    check(items.map(each => each.child.number + ":" + each.child.round).join(), [0, 1, 2].map(i => i + ":" + round).join(), "young objects stored in a loop");

    let longer = makeItem(4, "l");
    longer.extra = "e";
    setFlags(longer, 5);
    check(describe(longer), "tag=4,key=l,child=null,sibling=null,flags=5,extra=e", "with a property added later");

    let reversed = build(["flags", 1], ["sibling", 2], ["child", 3], ["key", 4], ["tag", 5]);
    setFlags(reversed, "r");
    setChild(reversed, "c");
    check(describe(reversed), "flags=r,sibling=2,child=c,key=4,tag=5", "the names in the other order");

    let without = build(["tag", 1], ["key", 2]);
    setFlags(without, 9);
    check(describe(without), "tag=1,key=2,flags=9", "the property is not there: it is added");

    let other = build(["a", 1], ["b", 2], ["c", 3], ["d", 4], ["e", 5]);
    setFlags(other, 9);
    check(describe(other), "a=1,b=2,c=3,d=4,e=5,flags=9", "another name is in the slot");

    let readOnly = makeItem(1, "k");
    Object.defineProperty(readOnly, "flags", { writable: false });
    setFlags(readOnly, 7);
    check(readOnly.flags, 0, "read-only: ignored");
    throwsTypeError(() => setFlagsStrictly(readOnly, 7), "read-only, strictly");
    check(readOnly.flags, 0, "read-only: still");

    let seen = [];
    let accessor = makeItem(1, "k");
    Object.defineProperty(accessor, "flags", { get() { return "got"; }, set(value) { seen.push(value); }, enumerable: true, configurable: true });
    setFlags(accessor, 7);
    setFlagsStrictly(accessor, 8);
    check(seen.join() + accessor.flags, "7,8got", "an accessor");

    let hidden = makeItem(1, "k");
    Object.defineProperty(hidden, "flags", { enumerable: false });
    setFlags(hidden, 7);
    check(hidden.flags + describe(hidden), "7tag=1,key=k,child=null,sibling=null", "not enumerable: stored, still not enumerable");

    let frozen = Object.freeze(makeItem(1, "k"));
    setFlags(frozen, 7);
    throwsTypeError(() => setFlagsStrictly(frozen, 7), "frozen, strictly");
    check(frozen.flags, 0, "frozen");

    let sealed = Object.seal(makeItem(1, "k"));
    setFlags(sealed, 7);
    check(sealed.flags, 7, "sealed: stored");
    check(Object.getOwnPropertyDescriptor(sealed, "flags").configurable, false, "sealed: still");

    let closed = Object.preventExtensions(makeItem(1, "k"));
    setFlags(closed, 7);
    check(closed.flags, 7, "not extensible: stored");
    let closedWithout = Object.preventExtensions(build(["tag", 1]));
    setFlags(closedWithout, 7);
    throwsTypeError(() => setFlagsStrictly(closedWithout, 7), "not extensible and absent, strictly");
    check(describe(closedWithout), "tag=1", "not extensible and absent");

    let deleted = makeItem(1, "k");
    delete deleted.flags;
    setFlags(deleted, 7);
    check(describe(deleted), "tag=1,key=k,child=null,sibling=null,flags=7", "deleted, then stored");
    let moved = makeItem(1, "k");
    delete moved.tag;
    moved.tag = "late";
    setFlags(moved, 7);
    setTagAndSibling(moved, "t", "s");
    check(describe(moved), "key=k,child=null,sibling=s,flags=7,tag=t", "another one deleted and added again");

    let dictionary = makeItem(1, "k");
    for (let i = 0; i < 80; ++i)
        dictionary["p" + i] = i;
    for (let i = 0; i < 80; ++i)
        delete dictionary["p" + i];
    setFlags(dictionary, 7);
    check(describe(dictionary), "tag=1,key=k,child=null,sibling=null,flags=7", "a dictionary");

    let inheritsSetter = Object.create(withSetter("flags", value => { seen.push("inherited " + value); }));
    setFlags(inheritsSetter, 1);
    check(seen.pop() + Object.keys(inheritsSetter).length, "inherited 10", "absent, and a setter is inherited");
    let ownOverSetter = makeItem(1, "k");
    Object.setPrototypeOf(ownOverSetter, withSetter("flags", value => { seen.push("never"); }));
    setFlags(ownOverSetter, 3);
    check(ownOverSetter.flags + ":" + seen.includes("never"), "3:false", "an own property wins over an inherited setter");
    let inheritsReadOnly = Object.create(Object.freeze(build(["flags", "fixed"])));
    setFlags(inheritsReadOnly, 1);
    throwsTypeError(() => setFlagsStrictly(inheritsReadOnly, 1), "absent, and a read-only one is inherited, strictly");
    check(inheritsReadOnly.flags + Object.keys(inheritsReadOnly).length, "fixed0", "absent, and a read-only one is inherited");

    let array = keep([1, 2]);
    setFlags(array, 7);
    check(array.flags + ":" + array.length, "7:2", "an array");
    let f = keep(function () { });
    setFlags(f, 7);
    check(f.flags, 7, "a function");
    let trapped = [];
    setFlags(new Proxy({ }, { set(target, name, value) { trapped.push(name + "=" + value); return true; } }), 7);
    check(trapped.join(), "flags=7", "a proxy");

    for (let primitive of [5, "string", true, Symbol.iterator, 10n]) {
        setFlags(primitive, 1);
        throwsTypeError(() => setFlagsStrictly(primitive, 1), "a primitive, strictly: " + typeof primitive);
    }
    for (let nothing of [null, undefined]) {
        throwsTypeError(() => setFlags(nothing, 1), "no object");
        throwsTypeError(() => setFlagsStrictly(nothing, 1), "no object, strictly");
    }

    setNeverBorn(item, round);
    check(item.neverBornAnywhere, round, "a name that no shape has");
    check(describe(bornHere(round)), "first=1,second=" + round, "an object born in the function");
    check(describe(defines(round)), "first=" + round + ",second=" + round, "definitions");
    let born = keep(new Born(1, round));
    check(describe(born), "bornFirst=" + round + ",bornSecond=" + round, "stores to this in a constructor");
    setBornSecond(born, -round);
    check(born.bornSecond, -round, "a store to an instance of a class");
}

for (let round = 0; round < 40; ++round) {
    let parent = makeItem("parent", "k");
    let readsFlags = o => o.flags;
    let child = Object.create(parent);
    for (let i = 0; i < 20; ++i)
        check(readsFlags(child), 0, "inherited");
    setFlags(parent, round + 1);
    check(readsFlags(child), round + 1, "stored to an object that became a prototype: the heir sees it");
    parent.added = 1;
    parent.flags = -1;
    setFlags(parent, round + 2);
    check(readsFlags(child) + ":" + child.added, round + 2 + ":1", "the same after a property was added to the prototype");
    let lateParent = keep({ });
    let lateChild = Object.create(lateParent);
    lateParent.tag = 1; lateParent.key = 2; lateParent.child = 3; lateParent.sibling = 4; lateParent.flags = 5;
    for (let i = 0; i < 20; ++i)
        check(readsFlags(lateChild), 5, "inherited from an object filled after it became a prototype");
    setFlags(lateParent, round);
    check(readsFlags(lateChild), round, "stored to it");
}

for (let round = 0; round < 40; ++round) {
    let named = keep(new Named(() => "first"));
    for (let i = 0; i < 20; ++i)
        check(String(named) + `${named}`, "firstfirst", "its own toString");
    setToString(named, () => "second " + round);
    check(String(named) + `${named}`, "second " + round + "second " + round, "toString replaced after conversions to a string were cached");
    let heir = Object.create(named);
    for (let i = 0; i < 20; ++i)
        check(String(heir), "second " + round, "an inherited toString");
    setToString(named, () => "third");
    check(String(heir) + String(named), "thirdthird", "replaced on the prototype");
}

{
    let old = [];
    for (let i = 0; i < 200; ++i)
        old.push(makeItem(i, "old"));
    if (typeof fullGC === "function")
        fullGC();
    for (let round = 0; round < 30; ++round) {
        for (let i = 0; i < old.length; ++i)
            setChild(old[i], { number: i, round, text: "young " + i });
        setChildOfAll(old, i => ({ number: i, round, text: "young " + i }));
        if (typeof edenGC === "function")
            edenGC();
        let garbage = [];
        for (let i = 0; i < 300; ++i)
            garbage.push({ a: i, b: "g" + i });
        for (let i = 0; i < old.length; ++i)
            check(old[i].child.number + old[i].child.text + old[i].child.round, i + "young " + i + round, "a young object stored into an old one survives");
    }
}

if (typeof aotRemarks === "function" && aotRemarks("check") && jscOptions().useAOTDataStubs) {
    for (const name of ["bump"])
        check(aotRemarks(name).includes("guards-over-whole-function"), true, name + " has guards over the whole function");
    for (const name of jscOptions().useAOTGuardsAtEveryGuessedPlaceForTesting ? ["setChild", "setFlags", "setFlagsStrictly", "setSelf", "setTagAndSibling", "setBornSecond"] : [])
        check(aotRemarks(name).includes("guards-over-whole-function"), true, name + " has guards over the whole function");
}
