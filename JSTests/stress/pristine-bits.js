//@ $skipModes << :lockdown # every array has slow-put array storage there, and no such array has pristine elements.
//@ requireOptions("--useDollarVM=1")
// The two pristine bits of a Structure: what marking needs, which transitions keep them, and what marking does to the Structures and
// the inline caches that exist already. And the flag in the cell, the one thing that compiled code tests: of an array it says that the
// Structure has both bits, of another object that it has the first, and a cell from before the mark keeps it clear.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + String(expected) + " but got " + String(actual));
}

function bits(object) {
    let names = $vm.hasPristineNames(object), elements = $vm.hasPristineElements(object);
    if ($vm.isPristine(object))
        shouldBe(names && (elements || !Array.isArray(object)), true, "the flag in the cell says no more than the Structure");
    return names && elements ? "both" : names ? "names" : elements ? "elements" : "none";
}

let arrayOfEachIndexingMode = {
    ArrayWithUndecided: "[]", ArrayWithInt32: "Array.of(1)", ArrayWithDouble: "Array.of(1.5)", ArrayWithContiguous: "Array.of('a')",
    CopyOnWriteArrayWithInt32: "[1].slice()", CopyOnWriteArrayWithDouble: "[1.5].slice()", CopyOnWriteArrayWithContiguous: "['a'].slice()",
    ArrayWithArrayStorage: "(a => (a[100000] = 1, a))([])",
};
let arrayOfEachStructure = Object.values(arrayOfEachIndexingMode);
let createArrayOfStrings = "(function () { let string = 'a'; return [string]; })";

function createLockedRealm() {
    let realm = $vm.createGlobalObject();
    $vm.makePropertiesImmutable(realm.Object.prototype);
    $vm.makePropertiesImmutable(realm.Array.prototype);
    return realm;
}

function createMarkedRealm() {
    let realm = createLockedRealm();
    for (let indexingMode in arrayOfEachIndexingMode) {
        let array = realm.eval(arrayOfEachIndexingMode[indexingMode]);
        shouldBe($vm.indexingMode(array), indexingMode, arrayOfEachIndexingMode[indexingMode]);
        shouldBe($vm.tryMarkPristine(array), true, indexingMode);
    }
    return realm;
}

// Marking needs a chain of immutable prototypes. An array from before the mark keeps a clear flag.
{
    let realm = $vm.createGlobalObject();
    shouldBe($vm.tryMarkPristine(realm.eval("[]")), false, "both prototypes are mutable");
    $vm.makePropertiesImmutable(realm.Object.prototype);
    shouldBe($vm.tryMarkPristine(realm.eval("[]")), false, "Array.prototype is mutable");
    $vm.makePropertiesImmutable(realm.Array.prototype);
    let before = realm.eval("[]");
    shouldBe(bits(before), "none", "before the mark");
    shouldBe($vm.tryMarkPristine(realm.eval("[]")), true, "the chain is immutable");
    shouldBe(bits(realm.eval("[]")), "both", "after the mark");
    shouldBe($vm.isPristine(before), false, "an array from before the mark");
    shouldBe(bits([]), "none", "another realm");
}

let realm = createMarkedRealm();

// Arrays of strings and of objects, however they are made. The realm derives the Structures of match arrays from the Structure of
// these arrays when it is created, so that Structure has transitions before anybody can mark it.
for (let source of [
    "['a']", "['a'].slice()", "[{ }]", "[[]]", "[null, 'a']", "'a,b'.split(',')", "Object.keys({ a: 1 })", "JSON.parse('[\"a\", { }]')",
    "Array.from('ab')", "Array.of('a')", "['a'].map(x => x)", "['a'].concat(['b'])", "[...'ab']",
    "(a => (a.push('a'), a))([])", "(a => (a.push('a'), a))([1])", "(a => (a.push('a'), a))([1.5])", "(a => (a[0] = 'a', a))(new Array(2))",
    "/a/.exec('a')", "/a/d.exec('a')", "/a/d.exec('a').indices", "'a'.match(/a/)",
])
    shouldBe(bits(realm.eval(source)), "both", source);

let effects = {
    both: [
        "a.foo = 1", "a.foo = 1; delete a.foo", "a.foo = 1; Object.defineProperty(a, 'foo', { enumerable: false })", "a.foo = 1; a.bar = 2",
        "a.index = 0", "a.index = 0; a.input = 'a'", "a.index = 0; a.input = 'a'; a.groups = undefined", "a.groups = undefined", "a.indices = 1",
        "a[100000] = 1", "a.push('a')", "a.push(1.5, { })", "Object.create(a)", "Object.preventExtensions(a)",
    ],
    elements: ["a.map = 1", "a.constructor = 1", "a.hasOwnProperty = 1", "a[Symbol()] = 1", "a[Symbol.iterator] = function () { }", "a.then = 1", "a.toJSON = 1", "a.foo = 1; a.map = 1"],
    names: ["a['-1'] = 1", "a['1.5'] = 1", "a.foo = 1; a['-1'] = 1"],
    none: [
        "Object.setPrototypeOf(a, null)", "Object.setPrototypeOf(a, Object.prototype)", "Object.setPrototypeOf(a, Object.create(Array.prototype))",
        "Object.defineProperty(a, 0, { get() { } })", "for (let i = 0; i < 200; ++i) a['p' + i] = i", "a.map = 1; a['-1'] = 1",
    ],
};
for (let expected in effects) {
    for (let statements of effects[expected]) {
        for (let array of arrayOfEachStructure) {
            let changed = realm.eval(`(a => { ${statements}; return a; })(${array})`);
            shouldBe(bits(changed), expected, `${statements} on ${array}`);
            shouldBe($vm.isPristine(changed), expected == "both", `the flag in the cell after ${statements} on ${array}`);
        }
    }
}

shouldBe(bits(realm.eval("new (class A extends Array { })")), "none", "a subclass");
shouldBe(bits(realm.eval("Reflect.construct(Array, [], Object)")), "none", "another new.target");
shouldBe(bits(realm.eval("Object.seal([])")), "both", "sealed");
shouldBe(bits(realm.eval("Object.freeze([])")), "both", "frozen");

function compile(realm, statements) {
    return realm.eval(`(function (a) { ${statements}; return a; })`);
}

// Inline caches filled after the mark: a transition that loses a bit is not cached in any tier.
for (let [statements, expected] of [["a.tag = 1; a.other = 1", "both"], ["a.tag = 1; a.index = 1", "both"], ["a.tag = 1; a.map = 1", "elements"], ["a.tag = 1; a.then = 1", "elements"], ["a.tag = 1; a['-1'] = 1", "names"]]) {
    let store = compile(realm, statements), create = realm.eval(createArrayOfStrings);
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(bits(store(create())), expected, statements);
}

// Inline caches filled before the mark replay their transition without touching the flags of the cell. Marking reaches the Structures
// that exist already, and a bit that one of their transitions would lose is given to none of them.
function markLate(statements, expected) {
    let realm = createLockedRealm();
    let store = compile(realm, statements), create = realm.eval(createArrayOfStrings);
    let hasNames = expected == "both" || expected == "names", hasElements = expected == "both" || expected == "elements";
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(bits(store(create())), "none", statements);
    shouldBe($vm.tryMarkPristine(create()), hasNames, "marking after " + statements);
    for (let i = 0; i < 100; ++i)
        shouldBe(bits(store(realm.eval("Array.of('a')"))), expected, statements + " after the mark");
    for (let i = 0; i < testLoopCount; ++i) {
        let array = store(create());
        shouldBe($vm.isPristine(array) && !hasNames, false, statements + " in compiled code after the mark");
        shouldBe($vm.hasPristineElements(array), hasElements, statements + " in compiled code after the mark");
    }
}
markLate("a.tag = 1; a.other = 1", "both");
markLate("a.tag = 1; delete a.tag", "both");
markLate("a.tag = 1; a.other = 1; a.third = 1", "both");
markLate("a.tag = 1; a.map = 1", "elements");
markLate("a.tag = 1; a.other = 1; a.then = 1", "elements");
markLate("a.tag = 1; a[Symbol.iterator] = 1", "elements");
markLate("a.tag = 1; a['-1'] = 1", "names");
markLate("a.tag = 1; a.map = 1; a['-1'] = 1", "none");

// A change of prototype is never replayed by a cache, so it costs the other arrays nothing.
{
    let realm = createLockedRealm();
    let change = compile(realm, "Object.setPrototypeOf(a, null)"), create = realm.eval(createArrayOfStrings);
    for (let i = 0; i < testLoopCount; ++i)
        change(create());
    shouldBe($vm.tryMarkPristine(create()), true, "marking after a change of prototype");
    shouldBe(bits(realm.eval("Array.of('a')")), "both", "after a change of prototype");
    shouldBe(bits(change(realm.eval("Array.of('a')"))), "none", "the prototype is null");
}

// A Structure that is not the first of its class: the names it has count.
{
    let realm = createLockedRealm();
    let shadowing = realm.eval("(a => (a.map = 1, a))(Array.of('a'))");
    shouldBe($vm.tryMarkPristine(shadowing), false, "an own name that shadows");
    shouldBe(bits(shadowing), "elements", "an own name that shadows");
    let numeric = realm.eval("(a => (a['-1'] = 1, a))(Array.of(1))");
    shouldBe($vm.tryMarkPristine(numeric), true, "an own numeric name");
    shouldBe(bits(numeric), "names", "an own numeric name");
    let dictionary = realm.eval("(a => { for (let i = 0; i < 200; ++i) a['p' + i] = i; return a; })(Array.of(1.5))");
    shouldBe($vm.tryMarkPristine(dictionary), false, "a dictionary");
    shouldBe(bits(dictionary), "none", "a dictionary");
}
