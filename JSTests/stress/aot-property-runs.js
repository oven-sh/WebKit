//@ runDefault("--compileMainScriptAheadOfTime=1", "--useDollarVM=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function remarksOf(f) {
    let remarks = aotRemarks(f.name);
    if (!remarks && isAOTCompiled(f))
        throw new Error("no remarks for " + f.name);
    return remarks;
}
function matches(remark, pattern) { return remark === pattern || remark.startsWith(pattern + ":"); }
function applies(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (!remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " does not apply to " + f.name + ": " + remarks.join(" "));
    }
}
function doesNotApply(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " applies to " + f.name + ": " + remarks.join(" "));
    }
}
function describe(o) { return Object.keys(o).map(k => k + "=" + (typeof o[k] == "function" ? "f" : typeof o[k] == "object" ? "o" : o[k])).join(); }
function outcome(f, ...args) { try { return describe(f(...args)); } catch (e) { return e.constructor.name; } }
function other() { return 9; }

function four(o) { o.a = 1; o.b = 2; o.c = 3; o.d = 4; return o; }
function onlyEverAdds(o) { o.k = 1; o.l = 2; o.m = 3; o.n = 4; return o; }
function addsUntilThePrototypeChanges(o) { o.q = 1; o.r = 2; o.s = 3; o.t = 4; return o; }
function fourStrict(o) { "use strict"; o.a = 1; o.b = 2; o.c = 3; o.d = 4; return o; }
function closures(o) { o.a = () => 1; o.b = function () { return 2; }; o.c = async () => 3; o.d = () => 4; o.e = function* () { }; return o; }
function captured(o) { o.a = () => 1; o.b = () => 2; o.c = () => 3; o.d = () => o; return o; }
function literals(o, x) { o.a = { x }; o.b = { y: x, z: 1 }; o.c = x; o.d = "s"; o.e = { m() { return super.toString; } }; return o; }
function fresh(x) { let o = {}; o.a = x; o.b = x; o.c = x; o.d = x; o.e = x; return o; }
function many(o) { o.p0 = 0; o.p1 = 1; o.p2 = 2; o.p3 = 3; o.p4 = 4; o.p5 = 5; o.p6 = 6; o.p7 = 7; o.p8 = 8; o.p9 = 9; o.p10 = 10; o.p11 = 11; o.p12 = 12; o.p13 = 13; o.p14 = 14; o.p15 = 15; o.p16 = 16; o.p17 = 17; o.p18 = 18; o.p19 = 19; return o; }
function twoRuns(o, p) { o.a = 1; o.b = 2; o.c = 3; o.d = 4; p.a = 5; p.b = 6; p.c = 7; p.d = 8; return o; }

function three(o) { o.a = 1; o.b = 2; o.c = 3; return o; }
function callBetween(o) { o.a = 1; o.b = 2; o.c = other(); o.d = 4; o.e = 5; return o; }
function readBetween(o) { o.a = 1; o.b = 2; o.c = o.a; o.d = 4; o.e = 5; return o; }
function interleaved(o, p) { o.a = 1; p.a = 2; o.b = 3; p.b = 4; o.c = 5; p.c = 6; o.d = 7; p.d = 8; return o; }
function sameName(o) { o.a = 1; o.b = 2; o.a = 3; o.c = 4; o.d = 5; return o; }
function setsProto(o, p) { o.a = 1; o.b = 2; o.__proto__ = p; o.c = 3; o.d = 4; return o; }
function inLoop(o) { for (let i = 0; i < 2; ++i) { o.a = i; o.b = i; o.c = i; o.d = i; } return o; }
function inTry(o) { try { o.a = 1; o.b = 2; o.c = 3; o.d = 4; } catch { } return o; }
function straddles(o) { let where = "before"; o.a = 1; o.b = 2; try { where = "inside"; o.c = 3; o.d = 4; o.e = 5; o.f = 6; } catch (e) { return where + " caught " + e.constructor.name + " " + describe(o); } return describe(o); }
function assignsBetween(o) { let x = "old"; try { o.a = 1; x = "new"; o.b = 2; o.c = 3; o.d = 4; } catch { return x + " " + Object.keys(o).join(""); } return x; }
function fromVariables(o) { let v = 1, w = 2; let keep = () => v + w + o.a; o.a = v; o.b = w; o.c = v; o.d = w; return keep() + describe(o); }
function tooEarly(o) { let keep = () => late; o.a = 1; o.b = 2; o.c = late; o.d = 4; o.e = 5; let late = 3; return keep; }
function reassigned(o, p) { let keep = () => o; o.a = 1; o.b = 2; o = p; o.c = 3; o.d = 4; return keep; }
function computed(o, k) { o.a = 1; o.b = 2; o[k] = 3; o.c = 4; o.d = 5; return o; }

for (let round = 0; round < 3; ++round) {
    check(describe(four({})), "a=1,b=2,c=3,d=4", "an empty object");
    check(describe(four({ z: 0 })), "z=0,a=1,b=2,c=3,d=4", "an object with a property");
    check(describe(four({ c: 0 })), "c=3,a=1,b=2,d=4", "one of them is there already");
    check(describe(four({ d: 0, c: 0, b: 0, a: 0 })), "d=4,c=3,b=2,a=1", "all of them are");
    check(describe(four(Object.create(null))), "a=1,b=2,c=3,d=4", "no prototype");
    check(describe(four([])), "a=1,b=2,c=3,d=4", "an array");
    check(describe(four(function () { })), "a=1,b=2,c=3,d=4", "a function");
    check(describe(four(new Map)), "a=1,b=2,c=3,d=4", "a map");
    check(describe(closures({})), "a=f,b=f,c=f,d=f,e=f", "closures");
    let withClosures = captured({});
    check(describe(withClosures), "a=f,b=f,c=f,d=f", "closures, one of which captures the object");
    check(withClosures.d(), withClosures, "what it captured");
    check(describe(literals({}, 7)), "a=o,b=o,c=7,d=s,e=o", "literals");
    check(literals({}, 7).b.y, 7, "what a literal holds");
    check(literals({}, 7).e.m(), Object.prototype.toString, "a method with a home object");
    check(describe(fresh(1)), "a=1,b=1,c=1,d=1,e=1", "an object created there");
    check(Object.keys(many({})).length, 20, "more than fit inline");
    check(many({ x: 1, y: 2 }).p19, 19, "the last of many");
    check(describe(many(many({}))).length, describe(many({})).length, "again on the same object");
    let p = {};
    check(describe(twoRuns({}, p)) + ";" + describe(p), "a=1,b=2,c=3,d=4;a=5,b=6,c=7,d=8", "one object after another");
    let same = {};
    check(describe(twoRuns(same, same)), "a=5,b=6,c=7,d=8", "the same object twice");

    check(outcome(four, undefined), "TypeError", "undefined");
    check(outcome(four, null), "TypeError", "null");
    check(outcome(four, 5), "", "a number");
    check(outcome(fourStrict, 5), "TypeError", "a number, in strict mode");
    check(outcome(four, "s"), "0=s", "a string");
    check(outcome(four, Object.freeze({})), "", "a frozen object");
    check(outcome(fourStrict, Object.freeze({})), "TypeError", "a frozen object, in strict mode");
    check(outcome(four, Object.freeze({ b: 0 })), "b=0", "a frozen object that has one");
    check(outcome(four, Object.preventExtensions({ c: 0 })), "c=3", "an object that cannot be extended");
    let partly = Object.preventExtensions({ c: 0 });
    check(outcome(fourStrict, partly) + describe(partly), "TypeErrorc=0", "the same in strict mode stops at the first");
    let readOnly = Object.defineProperty({}, "c", { value: "fixed", enumerable: true });
    check(outcome(four, readOnly), "c=fixed,a=1,b=2,d=4", "a read-only property");
    readOnly = Object.defineProperty({}, "c", { value: "fixed", enumerable: true });
    check(outcome(fourStrict, readOnly) + describe(readOnly), "TypeErrorc=fixed,a=1,b=2", "a read-only property, in strict mode: the ones before it are stored");

    let log = [];
    let withSetter = { set c(v) { log.push("c=" + v + " sees " + Object.keys(this).join("")); } };
    four(withSetter);
    check(log.join(), "c=3 sees cab", "its own setter sees what was stored before it");
    log = [];
    let inherits = Object.create({ set b(v) { log.push("b=" + v + " sees " + Object.keys(this).join("")); } });
    check(describe(four(inherits)), "a=1,c=3,d=4", "an inherited setter");
    check(log.join(), "b=2 sees a", "which sees what was stored before it");
    let throws = Object.create({ set c(v) { throw new RangeError; } });
    check(outcome(four, throws) + describe(throws), "RangeErrora=1,b=2", "a setter that throws");
    let inheritsReadOnly = Object.create(Object.freeze({ b: "fixed" }));
    check(describe(four(inheritsReadOnly)), "a=1,c=3,d=4", "an inherited read-only property");
    log = [];
    four(new Proxy({}, { set(t, k, v) { log.push(k + "=" + v); return true; } }));
    check(log.join(), "a=1,b=2,c=3,d=4", "a proxy");
    log = [];
    four(Object.create(new Proxy({}, { set(t, k, v, r) { log.push(k + "=" + v); return Reflect.set(t, k, v, r); } })));
    check(log.join(), "a=1,b=2,c=3,d=4", "a proxy as the prototype");

    let deleted = { x: 1, y: 2, z: 3 };
    delete deleted.y;
    check(describe(four(deleted)), "x=1,z=3,a=1,b=2,c=3,d=4", "after a property was deleted");
    let dictionary = {};
    for (let i = 0; i < 200; ++i)
        dictionary["k" + i] = i;
    check(Object.keys(four(dictionary)).slice(-4).join(), "a,b,c,d", "an object with very many properties");
    let prototype = {};
    let child = Object.create(prototype);
    function readC(o) { return o.c; }
    for (let i = 0; i < 5; ++i)
        check(readC(child), undefined, "not there yet");
    four(prototype);
    check(readC(child), 3, "a property that appears on the prototype");

    check(describe(three({})), "a=1,b=2,c=3", "three");
    check(describe(callBetween({})), "a=1,b=2,c=9,d=4,e=5", "a call in between");
    check(describe(readBetween({})), "a=1,b=2,c=1,d=4,e=5", "a read in between");
    let q = {};
    check(describe(interleaved({}, q)) + ";" + describe(q), "a=1,b=3,c=5,d=7;a=2,b=4,c=6,d=8", "two objects in turn");
    same = {};
    check(describe(interleaved(same, same)), "a=2,b=4,c=6,d=8", "which are the same object");
    check(describe(sameName({})), "a=3,b=2,c=4,d=5", "the same name twice");
    let parent = { inherited: 1 };
    check(Object.getPrototypeOf(setsProto({}, parent)), parent, "__proto__");
    check(describe(inLoop({})), "a=1,b=1,c=1,d=1", "in a loop");
    check(describe(inTry({})), "a=1,b=2,c=3,d=4", "in a try block");
    check(describe(computed({}, "k")), "a=1,b=2,k=3,c=4,d=5", "a computed name in between");
    check(straddles({}), "a=1,b=2,c=3,d=4,e=5,f=6", "a try block begins half way");
    check(straddles(Object.create({ set d(v) { throw new RangeError; } })), "inside caught RangeError a=1,b=2,c=3", "a store inside the try block throws");
    let outside = "not thrown";
    try { straddles(Object.create({ set b(v) { throw new RangeError; } })); } catch (e) { outside = e.constructor.name; }
    check(outside, "RangeError", "a store before the try block throws");
    check(assignsBetween({}), "new", "an assignment in between");
    check(assignsBetween(Object.create({ set a(v) { throw 1; } })), "old ", "the first store throws before the assignment");
    check(assignsBetween(Object.create({ set b(v) { throw 1; } })), "new a", "the second store throws after it");
    check(fromVariables({}), "4a=1,b=2,c=1,d=2", "values from captured variables");
    let early = {};
    check(outcome(tooEarly, early) + describe(early), "ReferenceErrora=1,b=2", "a variable that is not initialized yet");
    let first = {}, second = {};
    reassigned(first, second);
    check(describe(first) + ";" + describe(second), "a=1,b=2;c=3,d=4", "the variable that holds the object is assigned to");
}
let kept = [];
for (let i = 0; i < 3000; ++i) {
    let o = {};
    o["unique" + i] = i;
    kept.push(many(four(o)));
    if (!(i % 1000))
        gc();
}
check(kept[2999].p19 + kept[0].d + kept[1500].unique1500, 19 + 4 + 1500, "many starting shapes, and collections");
kept = null;
gc();
check(describe(four({})), "a=1,b=2,c=3,d=4", "after the shapes died");

applies(four, "property-run:4", "calls:operationAOTPutProperties");
applies(fourStrict, "property-run:4");
applies(onlyEverAdds, "property-run:4");
applies(addsUntilThePrototypeChanges, "property-run:4");
applies(closures, "property-run:5");
applies(literals, "property-run:5");
applies(fresh, "property-run:5");
applies(many, "property-run:20");
applies(twoRuns, "property-run:4");
applies(captured, "property-run:4");
applies(inTry, "property-run:4");
applies(straddles, "property-run:4");
applies(fromVariables, "property-run:4");
doesNotApply(straddles, "property-run:6");
for (let f of [three, callBetween, readBetween, interleaved, sameName, setsProto, inLoop, computed, assignsBetween, tooEarly, reassigned])
    doesNotApply(f, "property-run");

if (isAOTCompiled(four)) {
    let structuresBehind = o => $vm.getStructureTransitionList(o).length / 5;
    class UnrelatedGetter { get other() { return 1; } set another(v) { } }
    class SetterForC { set c(v) { } }
    class GetterForB { get b() { return 1; } }
    class Changes { get other() { return 1; } }
    let inOneStep = {
        "an empty object": () => ({}),
        "an object with properties": () => ({ y: 1, z: 2 }),
        "no prototype": () => Object.create(null),
        "a class with accessors under other names": () => new UnrelatedGetter,
        "a subclass of it": () => new (class extends UnrelatedGetter { }),
        "a prototype with a plain property of that name": () => Object.create({ l: "shadowed" }),
        "an accessor of its own under another name": () => ({ get other() { return 1; } }),
    };
    let oneByOne = {
        "a setter for one of the names": () => new SetterForC,
        "a getter without a setter for one of them": () => new GetterForB,
        "a read-only property of that name on the prototype": () => Object.create(Object.freeze({ b: 1 })),
        "a proxy on the chain": () => Object.create(new Proxy({}, {})),
        "one of the names is there already": () => ({ c: 0 }),
        "an array on the chain": () => Object.create([]),
    };
    for (let round = 0; round < 2; ++round) {
        for (let [what, make] of Object.entries(inOneStep))
            check(structuresBehind(onlyEverAdds(make())), 1, "in one step: " + what);
        for (let [what, make] of Object.entries(oneByOne))
            check(structuresBehind(four(make())) > 1, true, "one by one: " + what);
    }
    check(structuresBehind(addsUntilThePrototypeChanges(new Changes)), 1, "before the prototype changes");
    let seen = [];
    Object.defineProperty(Changes.prototype, "s", { set(v) { seen.push(v); } });
    check(describe(addsUntilThePrototypeChanges(new Changes)), "q=1,r=2,t=4", "a setter that appeared on the prototype later");
    check(seen.join(), "3", "is called");
    let usedAsPrototype = {};
    Object.create(usedAsPrototype);
    check(describe(four(usedAsPrototype)), "a=1,b=2,c=3,d=4", "an object that is a prototype");
}
