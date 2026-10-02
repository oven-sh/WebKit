// Built-ins ask whether an object still has only what its class gave it before they take a fast path. Making an object's properties
// immutable adds and changes no property, so a pristine object stays pristine: every result below equals an ordinary object's.
// An object that was NOT pristine before still has its own properties honoured.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + String(expected) + " but got " + String(actual));
}

function outcome(func, subject) {
    try {
        return JSON.stringify(func(subject));
    } catch (e) {
        return "throws " + e.constructor.name;
    }
}

// A use below runs its operation over six inputs, so this many calls of a use make the operation itself hot.
let hot = Math.max(3, Math.ceil(testLoopCount / 20));

function sameAsOrdinary(uses, makeOrdinary, label) {
    let ordinary = makeOrdinary(), subject = $vm.makePropertiesImmutable(makeOrdinary());
    for (let name in uses) {
        for (let i = 0; i < hot; ++i) { outcome(uses[name], ordinary); outcome(uses[name], subject); }
        shouldBe(outcome(uses[name], subject), outcome(uses[name], ordinary), label + " " + name);
    }
}

let inputs = ["a1b2", "zz9", "q", "77abc", "none", "5x"];
let input = i => inputs[i % inputs.length];
let overInputs = func => r => inputs.map((s, i) => func(r, s, i));

// A RegExp that is not global or sticky never has its lastIndex written, so everything works, as for an ordinary one.
let regExpUses = {
    replace: overInputs((r, s) => s.replace(r, "<$1>")), replaceFunction: overInputs((r, s) => s.replace(r, (m, d) => d * 2)), split: overInputs((r, s) => s.split(r)), match: overInputs((r, s) => s.match(r)),
    search: overInputs((r, s) => s.search(r)), test: overInputs((r, s) => r.test(s)), exec: overInputs((r, s) => r.exec(s)), matchAll: r => [..."a1".matchAll(r)], symbolReplace: overInputs((r, s) => r[Symbol.replace](s, "!")),
    lastIndex: r => r.lastIndex, source: r => r.source + "/" + r.flags, string: r => String(r),
};
sameAsOrdinary(regExpUses, () => /(\d)(x)?/, "RegExp");

// A global or sticky one: what has to write lastIndex fails, exactly as it does for a frozen RegExp, and lastIndex stays put.
for (let flags of ["g", "y", "gy"]) {
    let subject = $vm.makePropertiesImmutable(new RegExp("\\d", flags)), frozen = Object.freeze(new RegExp("\\d", flags));
    let uses = { replace: r => "1a2".replace(r, "x"), match: r => "1a2".match(r), test: r => r.test("1"), exec: r => r.exec("1"), search: r => "a1".search(r), split: r => "1a2".split(r), matchAll: r => [..."1a2".matchAll(r)].length };
    for (let name in uses) {
        for (let i = 0; i < hot; ++i) outcome(uses[name], subject);
        shouldBe(outcome(uses[name], subject), outcome(uses[name], frozen), flags + " " + name);
    }
    shouldBe(subject.lastIndex, 0);
}

// search has to write a lastIndex that is not 0, so it fails; with 0 it writes nothing.
{
    let regExp = /\d/; regExp.lastIndex = 3; $vm.makePropertiesImmutable(regExp);
    for (let i = 0; i < hot; ++i) shouldBe(outcome(r => input(i).search(r), regExp), "throws TypeError");
    shouldBe(regExp.lastIndex, 3);
}

// Not pristine: an own exec is honoured.
{
    let withOwnExec = () => { let r = /\d/; r.exec = () => { let m = ["own"]; m.index = 0; return m; }; return r; };
    sameAsOrdinary({ replace: overInputs((r, s) => s.replace(r, "x")), test: overInputs((r, s) => r.test(s)), match: overInputs((r, s) => s.match(r)), search: overInputs((r, s) => s.search(r)), split: r => "a1b".split(r) }, withOwnExec, "RegExp with its own exec");
    shouldBe($vm.makePropertiesImmutable(withOwnExec()).test("no digits"), true);
}

// Arrays.
let arrayUses = {
    join: a => a.join("-"), toString: a => a.toString(), string: a => String(a), template: a => `${a}`, plus: a => a + "", slice: a => a.slice(1), concat: a => a.concat([9]), map: a => a.map(x => x * 2), filter: a => a.filter(x => x > 1),
    flat: a => a.flat(), indexOf: a => a.indexOf(4), includes: a => a.includes(2), sliceIsArray: a => a.slice().constructor === Array, spread: a => [...a], from: a => Array.from(a), keys: a => Object.keys(a),
};
function numbers() { let a = []; for (let i = 1; i < 4; ++i) a.push(i); return a; }
sameAsOrdinary(arrayUses, numbers, "array");
sameAsOrdinary(arrayUses, () => [1, 2, 3], "array literal");
{
    // Not pristine: an own constructor's species and an own join are honoured.
    class Special extends Array { }
    let withSpecies = numbers(); withSpecies.constructor = Special; $vm.makePropertiesImmutable(withSpecies);
    for (let i = 0; i < hot; ++i) withSpecies.slice(1);
    shouldBe(withSpecies.slice(1) instanceof Special, true); shouldBe(withSpecies.map(x => x) instanceof Special, true); shouldBe(withSpecies.concat([1]) instanceof Special, true);
    let withJoin = numbers(); withJoin.join = () => "own join"; $vm.makePropertiesImmutable(withJoin);
    for (let i = 0; i < hot; ++i) String(withJoin);
    shouldBe(String(withJoin), "own join"); shouldBe(`${withJoin}`, "own join");
    // The realm's join is replaced: an immutable array follows it as an ordinary one does.
    let subject = $vm.makePropertiesImmutable(numbers()), ordinary = numbers(), join = Array.prototype.join;
    for (let i = 0; i < hot; ++i) String(subject);
    Array.prototype.join = function () { return "replaced"; };
    shouldBe(String(subject), "replaced"); shouldBe(String(ordinary), "replaced");
    Array.prototype.join = join;
    shouldBe(String(subject), "1,2,3");
}

// Other classes with such a fast path.
{
    let seen = [];
    let promise = $vm.makePropertiesImmutable(Promise.resolve(5));
    promise.then(v => seen.push(v)); Promise.resolve(5).then(v => seen.push(v)); (async () => seen.push(await promise))();
    drainMicrotasks();
    shouldBe(seen.join(), "5,5,5");

    function ordinary(a, b) { return a + b; }
    let subject = $vm.makePropertiesImmutable(function ordinary(a, b) { return a + b; });
    let boundOrdinary = ordinary.bind(null, 1), boundSubject = subject.bind(null, 1);
    shouldBe(boundSubject.name, boundOrdinary.name); shouldBe(boundSubject.length, boundOrdinary.length); shouldBe(boundSubject(2), 3); shouldBe(subject.name, "ordinary"); shouldBe(subject.length, 2);

    let args = $vm.makePropertiesImmutable((function () { "use strict"; return arguments; })(1, 2, 3));
    shouldBe(JSON.stringify([...args]), "[1,2,3]"); shouldBe(args.length, 3); shouldBe(Math.max(...args), 3);

    let iterator = $vm.makePropertiesImmutable(new Map([[1, 2]]).entries());
    shouldBe(JSON.stringify([...iterator]), "[[1,2]]");

    let record = $vm.makePropertiesImmutable({ a: 1, b: { c: 2 } });
    for (let i = 0; i < hot; ++i) { let copy = { ...record }; copy.z = i; shouldBe(copy.z, i); shouldBe($vm.hasImmutableProperties(copy), false); }
    shouldBe(JSON.stringify(Object.assign({}, record)), '{"a":1,"b":{"c":2}}');
}

// A RegExp without immutable properties takes the path it took. A frozen one from another realm, searched with this realm's
// RegExp.prototype[@@search], goes through that realm's exec, which records the match in that realm's RegExp.$1 and leaves this realm's alone.
{
    let other = $vm.createGlobalObject();
    let frozen = Object.freeze(new other.RegExp("(c)"));
    for (let i = 0; i < hot; ++i) {
        /(q)/.exec("q");
        new other.RegExp("(p)").exec("p");
        shouldBe(RegExp.prototype[Symbol.search].call(frozen, "abc"), 2);
        shouldBe(RegExp.$1, "q");
        shouldBe(other.RegExp.$1, "c");
    }
}
