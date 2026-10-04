//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlineGuessedPlacesEverywhere=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlineGuessedPlacesEverywhere=1", "--useAOTImmutableStructureAddresses=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlineGuessedPlacesEverywhere=1", "--validateGraphAtEachPhase=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlineGuessedPlacesEverywhere=1", "--useAOTInlining=0", "--validateGraphAtEachPhase=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTSearchOfPropertyNameIDs=1", "--useAOTSearchOfPropertyNameIDsInGetById=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTSearchOfPropertyNameIDs=1", "--useAOTSearchOfPropertyNameIDsInGetById=1", "--useAOTInlineGuessedPlacesEverywhere=1", "--validateGraphAtEachPhase=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTSearchOfPropertyNameIDs=1", "--useAOTInlining=0", "--validateGraphAtEachPhase=1", "--validateAOTInferredTypes=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuessedPlaces=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--useAOTOperationCounters=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--validateGraphAtEachPhase=1", "--validateAOTInferredTypes=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTDataStubs=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--verifyGC=1", "--scribbleFreeCells=1", "--useZombieMode=1", "--slowPathAllocsBetweenGCs=50")
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
const remarksOf = f => typeof aotRemarks === "function" && aotRemarks(f.name) || null;
const isGuessed = f => remarksOf(f).some(remark => remark.startsWith("guessed-place-read:") || remark.startsWith("guessed-place-read-through-stub:"));
const guesses = typeof jscOptions !== "function" || jscOptions().useAOTGuessedPlaces === undefined || !!jscOptions().useAOTGuessedPlaces;
function applies(...functions) {
    for (let f of functions) {
        if (remarksOf(f) && guesses && jscOptions().useAOTDataStubs && !isGuessed(f))
            throw new Error("no place is guessed in " + f.name + ": " + remarksOf(f).join(" "));
    }
}
function doesNotApply(...functions) {
    for (let f of functions) {
        if (remarksOf(f) && isGuessed(f))
            throw new Error("a place is guessed in " + f.name + ": " + remarksOf(f).join(" "));
    }
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
function makePair(first, second) { return keep({ first, second }); }
function build(...pairs) {
    let o = {};
    for (let [name, value] of pairs)
        o[name] = value;
    return keep(o);
}
function makeReversed(child, tag) { return build(["child", child], ["sibling", 0], ["tag", tag]); }
function makeTagSecond(tag) { return keep({ other: 1, tag }); }

function tagAndChild(x) { return x.tag + ":" + x.child; }
function keyAndFlags(x) { return x.key + ":" + x.flags; }
function firstAndSecond(x) { return x.first + ":" + x.second; }
function tagAlone(x) { return x.tag; }
function four(tag, key, child, flags) { return tag + ":" + child + "|" + key + ":" + flags; }
function allFour(x) { return four(x.tag, x.key, x.child, x.flags); }
function interleaved(x, y) { return four(x.tag, y.tag, x.key, y.key); }
function acrossBranches(x) {
    let tag = x.tag;
    if (tag === null)
        return "none";
    if (typeof tag === "string")
        return tag + "/" + x.key;
    return four(tag, x.key, x.child, x.flags);
}
function readsTagTwice(x) { return four(x.tag, x.key, x.tag, x.flags) + "|" + x.child; }
function readsTagTwiceInLoop(x, count) {
    let result = "";
    for (let i = 0; i < count; ++i)
        result += four(x.tag, x.tag, x.key, x.flags) + ";";
    return result;
}
function neverBorn(x) { return x.tag + ":" + x.nobodyIsBornWithThis; }

function Wide() {
    this.w0 = 0; this.w1 = 1; this.w2 = 2; this.w3 = 3; this.w4 = 4; this.w5 = 5; this.w6 = 6; this.w7 = 7;
    this.w8 = 8; this.w9 = 9; this.w10 = 10; this.w11 = 11; this.w12 = 12; this.w13 = 13; this.w14 = 14; this.w15 = 15;
    this.w16 = 16; this.w17 = 17; this.w18 = 18; this.w19 = 19; this.w20 = 20; this.w21 = 21; this.w22 = 22; this.w23 = 23;
    this.w24 = 24; this.w25 = 25; this.w26 = 26; this.w27 = 27; this.w28 = 28; this.w29 = 29;
}
function makeWide() { return keep(new Wide); }
function edgesOfWide(x) { return [x.w0, x.w15, x.w16, x.w23, x.w24, x.w29].join(); }

for (let f of [makeItem, makePair, build, makeReversed, makeTagSecond, tagAndChild, keyAndFlags, firstAndSecond, tagAlone, four, allFour, readsTagTwice, readsTagTwiceInLoop, interleaved, acrossBranches, neverBorn, makeWide, edgesOfWide, keep])
    noInline(f);

applies(tagAndChild, keyAndFlags, firstAndSecond, allFour, interleaved, acrossBranches, edgesOfWide);
for (let f of [allFour, interleaved, acrossBranches]) {
    if (remarksOf(f) && remarksOf(f).some(remark => remark.startsWith("guessed-place-read:")) && !remarksOf(f).includes("reuses-structure-of-base"))
        throw new Error("the reads of one value in " + f.name + " do not share its Structure: " + remarksOf(f).join(" "));
}
doesNotApply(tagAlone, neverBorn);

const cases = {
    "as it was born"(o) { return ["1:null", "k:0"]; },
    "a property is added"(o) { o.added = 1; o.alsoAdded = 2; return ["1:null", "k:0"]; },
    "a value is replaced"(o) { o.tag = 7; o.flags = 8; return ["7:null", "k:8"]; },
    "a property is deleted"(o) { delete o.tag; return ["undefined:null", "k:0"]; },
    "the first property is deleted and added again"(o) { delete o.tag; o.tag = 9; return ["9:null", "k:0"]; },
    "the last property is deleted and another takes its slot"(o) { delete o.flags; o.other = "other"; return ["1:null", "k:undefined"]; },
    "a property becomes an accessor"(o) { Object.defineProperty(o, "tag", { get() { return "got"; }, configurable: true }); return ["got:null", "k:0"]; },
    "an accessor becomes a value again"(o) {
        Object.defineProperty(o, "tag", { get() { return "got"; }, configurable: true });
        Object.defineProperty(o, "tag", { value: "again", writable: true, enumerable: true, configurable: true });
        return ["again:null", "k:0"];
    },
    "a property becomes read-only"(o) { Object.defineProperty(o, "key", { value: "fixed", writable: false }); return ["1:null", "fixed:0"]; },
    "a property becomes non-enumerable"(o) { Object.defineProperty(o, "key", { enumerable: false }); return ["1:null", "k:0"]; },
    "frozen"(o) { Object.freeze(o); return ["1:null", "k:0"]; },
    "sealed"(o) { Object.seal(o); return ["1:null", "k:0"]; },
    "not extensible"(o) { Object.preventExtensions(o); return ["1:null", "k:0"]; },
    "another prototype that has the names"(o) { Object.setPrototypeOf(o, { tag: "inherited", flags: "inherited" }); return ["1:null", "k:0"]; },
    "no prototype"(o) { Object.setPrototypeOf(o, null); return ["1:null", "k:0"]; },
    "deleted, with a prototype that has the name"(o) { Object.setPrototypeOf(o, { tag: "inherited" }); delete o.tag; return ["inherited:null", "k:0"]; },
    "a dictionary by many additions"(o) {
        for (let i = 0; i < 300; ++i)
            o["extra" + i] = i;
        return ["1:null", "k:0"];
    },
    "a dictionary by many deletions"(o) {
        for (let i = 0; i < 40; ++i)
            o["extra" + i] = i;
        for (let i = 0; i < 40; ++i)
            delete o["extra" + i];
        delete o.tag;
        o.tag = "late";
        return ["late:null", "k:0"];
    },
    "a dictionary that is flattened"(o) {
        for (let i = 0; i < 40; ++i)
            o["extra" + i] = i;
        for (let i = 0; i < 40; ++i)
            delete o["extra" + i];
        delete o.tag;
        delete o.key;
        o.key = "moved";
        if (typeof flattenDictionaryObject === "function")
            flattenDictionaryObject(o);
        keep(Object.create(o));
        for (let name in o) { }
        return ["undefined:null", "moved:0"];
    },
    "a flattened dictionary gets the name in the slot that another had"(o) {
        for (let i = 0; i < 40; ++i)
            o["extra" + i] = i;
        for (let i = 0; i < 40; ++i)
            delete o["extra" + i];
        delete o.tag;
        if (typeof flattenDictionaryObject === "function")
            flattenDictionaryObject(o);
        o.tag = "last";
        return ["last:null", "k:0"];
    },
    "used as a prototype"(o) { keep(Object.create(o)); return ["1:null", "k:0"]; },
    "an accessor that moves the properties read after it"(o) {
        Object.defineProperty(o, "key", {
            get() {
                delete this.child;
                delete this.flags;
                this.flags = "f";
                this.child = "c";
                return "g";
            },
            configurable: true,
        });
        o.key;
        return ["1:c", "g:f"];
    },
    "an accessor that deletes the properties read after it"(o) {
        Object.defineProperty(o, "key", { get() { delete this.child; delete this.flags; return "g"; }, configurable: true });
        o.key;
        return ["1:undefined", "g:undefined"];
    },
    "an accessor that makes the object a dictionary"(o) {
        Object.defineProperty(o, "key", {
            get() {
                for (let i = 0; i < 80; ++i)
                    this["extra" + i] = i;
                for (let i = 0; i < 80; ++i)
                    delete this["extra" + i];
                delete this.child;
                this.child = "late";
                return "g";
            },
            configurable: true,
        });
        o.key;
        return ["1:late", "g:0"];
    },
};
for (let round = 0; round < 4; ++round) {
    for (let [what, change] of Object.entries(cases)) {
        let o = makeItem(1, "k");
        check(tagAndChild(o), "1:null", what + ", before");
        check(keyAndFlags(o), "k:0", what + ", before");
        let [first, second] = change(o);
        for (let again = 0; again < 3; ++again) {
            check(tagAndChild(o), first, what);
            check(keyAndFlags(o), second, what);
            check(allFour(o), first + "|" + second, what + ", read in a row");
        }
        check(tagAndChild(makeItem(2, "l")), "2:null", what + ", another object afterwards");
    }
}

class WithAccessors {
    get tag() { return "getter"; }
    get child() { return "of the class"; }
}
class Derived extends Item {
    constructor() {
        super("derived", "d");
        this.more = 1;
    }
}
function withOwnPrototype() {
    class Local {
        constructor() {
            this.tag = "local";
            this.key = "l";
            this.child = null;
            this.sibling = null;
            this.flags = 0;
        }
    }
    return keep(new Local);
}
noInline(withOwnPrototype);
function tagged() { }
tagged.tag = "function";
tagged.child = "own";
const array = [1, 2];
array.tag = "array";
array.child = "own";
const others = [
    [makeReversed("c", "t"), "t:c", "the same names in other slots"],
    [keep({ tag: "only" }), "only:undefined", "one of the names"],
    [build(["a", 1], ["b", 2], ["c", 3], ["tag", "fourth"], ["child", "fifth"]), "fourth:fifth", "the names behind others"],
    [makeTagSecond("second"), "second:undefined", "one of the names, in another slot"],
    [keep({}), "undefined:undefined", "an empty object"],
    [keep(Object.create(makeItem("inherited", "k"))), "inherited:null", "both names inherited"],
    [keep(new WithAccessors), "getter:of the class", "accessors of a class"],
    [keep(new Derived), "derived:null", "an instance of a derived class"],
    [tagged, "function:own", "a function"],
    [array, "array:own", "an array"],
    [keep(new Proxy({}, { get(target, name) { return "proxy " + String(name); } })), "proxy tag:proxy child", "a proxy"],
    [keep(new Proxy(makeItem("target", "k"), {})), "target:null", "a proxy without traps"],
    [keep(Object.assign(new String("s"), build(["tag", "wrapper"], ["child", 0]))), "wrapper:0", "a String object"],
    [keep(Object.assign(new Map, build(["tag", "map"], ["child", 0]))), "map:0", "a Map"],
    [keep(Object.assign(/x/, build(["tag", "regexp"], ["child", 0]))), "regexp:0", "a RegExp"],
    [keep(Object.assign(Object.create(null), build(["tag", "bare"], ["child", 0]))), "bare:0", "an object without a prototype"],
    [keep(JSON.parse('{"tag":"parsed","key":1,"child":null,"sibling":null,"flags":0}')), "parsed:null", "the same names in the same order, parsed"],
    [keep(JSON.parse('{"child":1,"tag":"parsed"}')), "parsed:1", "parsed in another order"],
    [Math, "undefined:undefined", "an object with a static table"],
    [globalThis, "undefined:undefined", "the global object"],
    [5, "undefined:undefined", "a number"],
    [1.5, "undefined:undefined", "a double"],
    ["string", "undefined:undefined", "a string"],
    [true, "undefined:undefined", "a boolean"],
    [Symbol.iterator, "undefined:undefined", "a symbol"],
    [10n, "undefined:undefined", "a BigInt"],
];
for (let round = 0; round < 4; ++round) {
    for (let [value, expected, what] of others) {
        check(tagAndChild(value), expected, what);
        check(allFour(value).split("|")[0], expected, what + ", read in a row");
        check(tagAndChild(makeItem(3, "m")), "3:null", what + ", then the usual object");
    }
    for (let i = 0; i < 12; ++i)
        check(tagAndChild(withOwnPrototype()), "local:null", "instances whose classes differ");
    throwsTypeError(() => tagAndChild(undefined), "undefined");
    throwsTypeError(() => tagAndChild(null), "null");
    throwsTypeError(() => allFour(undefined), "undefined, read in a row");
    {
        let x = makeItem(1, "k"), y = makeItem(2, "l");
        check(interleaved(x, y), "1:k|2:l", "two values read in turn");
        check(interleaved(x, x), "1:k|1:k", "one value as both");
        check(interleaved(5, y), "undefined:undefined|2:l", "a number and an object read in turn");
        check(interleaved(x, "s"), "1:k|undefined:undefined", "an object and a string read in turn");
        throwsTypeError(() => interleaved(x, null), "an object and null read in turn");
        let changer = makeItem(3, "m");
        Object.defineProperty(changer, "tag", {
            get() {
                delete x.key;
                x.late = 0;
                x.key = "moved";
                return "got";
            },
            configurable: true,
        });
        for (let again = 0; again < 3; ++again)
            check(interleaved(x, changer), "1:moved|got:m", "the read of one value moves a property of the other");
        let remover = makeItem(4, "n");
        Object.defineProperty(remover, "tag", { get() { delete y.key; return "got"; }, configurable: true });
        for (let again = 0; again < 3; ++again)
            check(interleaved(y, remover), "2:undefined|got:n", "the read of one value deletes a property of the other");
        check(acrossBranches(makeItem(1, "k")), "1:null|k:0", "reads on both sides of two branches");
        check(acrossBranches(makeItem("s", "k")), "s/k", "reads on both sides of one branch");
        check(acrossBranches(makeItem(null, "k")), "none", "a read in front of a branch");
        check(acrossBranches(makeReversed("c", 7)), "7:c|undefined:undefined", "reads on both sides of two branches, other slots");
        check(acrossBranches(8), "undefined:undefined|undefined:undefined", "reads of a number on both sides of two branches");
    }
    for (let [value, tag, key, child, flags] of [[makeItem(1, "k"), 1, "k", null, 0], [makeReversed("c", 7), 7, undefined, "c", undefined], [5, undefined, undefined, undefined, undefined], ["s", undefined, undefined, undefined, undefined]]) {
        check(readsTagTwice(value), tag + ":" + tag + "|" + key + ":" + flags + "|" + child, "a property read twice between others");
        check(readsTagTwiceInLoop(value, 3), (tag + ":" + key + "|" + tag + ":" + flags + ";").repeat(3), "a property read twice in a loop, then others");
    }
    throwsTypeError(() => readsTagTwice(null), "null, a property read twice");
    check(firstAndSecond(makePair(1, 2)), "1:2", "a literal");
    check(firstAndSecond(build(["second", 2], ["first", 1])), "1:2", "the names of a literal in the other order");
    check(tagAlone(makeItem(4, "n")) + tagAlone(makeTagSecond(5)), 9, "a name that two shapes have in different slots");
    check(neverBorn(makeItem(6, "o")), "6:undefined", "with a name that no shape has");
    check(edgesOfWide(makeWide()), "0,15,16,23,24,29", "slots at the edges");
    let wide = makeWide();
    delete wide.w16;
    wide.w16 = "late";
    check(edgesOfWide(wide), "0,15,late,23,24,29", "slots at the edges, one moved");
}
Number.prototype.tag = "of numbers";
String.prototype.child = "of strings";
check(tagAndChild(5), "of numbers:undefined", "a number, inherited");
check(tagAndChild("string"), "undefined:of strings", "a string, inherited");
delete Number.prototype.tag;
delete String.prototype.child;

if (typeof aotOperationCount === "function" && remarksOf(tagAndChild) && jscOptions().useAOTOperationCounters && isGuessed(tagAndChild)) {
    const count = detail => aotOperationCount("operationAOTCountGuessedPlace" + (detail ? ":" + detail : "")) || 0;
    const details = ["hit", "not-a-cell", "not-an-object", "typed-layout", "dictionary", "not-an-own-property", "absent-and-every-name-is-recorded", "has-attributes", "out-of-line", "another-slot", "records-no-names", "slot-is-marked-nameless", "no-id-recorded"];
    check(details.reduce((sum, detail) => sum + count(detail), 0), count(), "every guessed read has one outcome");
    for (let detail of ["hit", "not-a-cell", "not-an-object", "dictionary", "not-an-own-property", "absent-and-every-name-is-recorded", "has-attributes", "another-slot"])
        check(count(detail) > 0, true, "some guessed read ends as " + detail);
    const before = [count(), count("hit")];
    const usual = makeItem(1, "k");
    for (let i = 0; i < 100; ++i)
        tagAndChild(usual);
    check(count() - before[0], 200, "guessed reads of an object as it was born");
    check(count("hit") - before[1], 200, "all of them hit");
}
