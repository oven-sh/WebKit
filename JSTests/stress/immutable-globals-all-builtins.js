//@ requireOptions("--immutableGlobals=*", "--useTemporal=1", "--useShadowRealm=1")

// --immutableGlobals=* makes every built-in of JavaScriptCore's own immutable (JSObject::makePropertiesImmutable()) in each new
// global object: constructors, prototypes, namespace objects and functions, with or without a global name, whenever they are created.
// It leaves the global object itself and what the embedder (here the jsc shell) adds to it as they are.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + String(expected) + " but got " + String(actual));
}

function shouldThrow(func, errorType, message) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof errorType))
        throw new Error((message ? message + ": " : "") + "expected " + errorType.name + " but got " + String(error));
}

// Values whose prototype chains lead to the built-ins that no property leads to.
const sourceOfValues = `[
    function* () { }, async function () { }, async function* () { },
    Object.getPrototypeOf((function* () { })()), Object.getPrototypeOf((async function* () { })()),
    [][Symbol.iterator](), new Map().entries(), new Set().values(), ""[Symbol.iterator](), "a".matchAll(/a/g),
    Iterator.from({ next() { } }), [].values().map(x => x),
    new Intl.Segmenter().segment("a"), new Intl.Segmenter().segment("a")[Symbol.iterator](),
    new Int8Array(1), (function () { return arguments; })(),
    Object.getOwnPropertyDescriptor((function () { "use strict"; return arguments; })(), "callee").get,
    new TypeError, new AggregateError([]), Temporal.Now.instant(),
    typeof WebAssembly === "object" ? new WebAssembly.Memory({ initial: 1 }) : null,
]`;

// Follows every own property (value, getter and setter) and every prototype from the properties of a global object.
function mutableObjectsReachableFrom(global, isExempt = () => false) {
    let seen = new Set([global, global.globalThis, global.$vm]);
    let queue = [];
    let mutable = [];
    function visit(value, path) {
        if ((typeof value !== "object" && typeof value !== "function") || value === null || seen.has(value))
            return;
        seen.add(value);
        queue.push([value, path]);
    }
    function visitProperties(object, path) {
        for (let key of Reflect.ownKeys(object)) {
            if (object === global && isExempt(key))
                continue;
            let descriptor = Reflect.getOwnPropertyDescriptor(object, key);
            visit(descriptor.value, path + String(key));
            visit(descriptor.get, "get " + path + String(key));
            visit(descriptor.set, "set " + path + String(key));
        }
    }
    visitProperties(global, "");
    global.eval(sourceOfValues).forEach((value, index) => {
        if (value !== null)
            visit(Object.getPrototypeOf(value), "value " + index + ".__proto__");
    });
    while (queue.length) {
        let [object, path] = queue.pop();
        if (!$vm.hasImmutableProperties(object))
            mutable.push(path);
        visit(Object.getPrototypeOf(object), path + ".__proto__");
        visitProperties(object, path + ".");
    }
    shouldBe(seen.size > 800, true);
    return mutable.join();
}

// A global object that has JavaScriptCore's built-ins only.
shouldBe(mutableObjectsReachableFrom($vm.createGlobalObject()), "");

// Deleting a property of the global object materializes all the others at once.
{
    let global = $vm.createGlobalObject();
    shouldBe(delete global.escape, true);
    shouldBe(mutableObjectsReachableFrom(global), "");
}

// This global object: the names a plain one has are the built-ins, the others are the shell's.
{
    let namesOfBuiltins = new Set(Reflect.ownKeys($vm.createGlobalObject()));
    namesOfBuiltins.delete("console"); // The shell replaces it.
    shouldBe(mutableObjectsReachableFrom(globalThis, key => !namesOfBuiltins.has(key)), "");
    shouldBe($vm.hasImmutableProperties(print), false);
    shouldBe($vm.hasImmutableProperties(performance), false);
    shouldBe($vm.hasImmutableProperties(performance.now), false);
    shouldBe($vm.hasImmutableProperties($), false);
    print.extra = 1;
    shouldBe(print.extra, 1);
}

// The global object is not immutable.
shouldBe($vm.hasImmutableProperties(globalThis), false);
var topLevelVar = 1;
let topLevelLet = 2;
globalThis.plainProperty = 3;
loadString("var lateVar = 4; let lateLet = 5; function lateFunction() { return 6; }");
shouldBe(topLevelVar + topLevelLet + plainProperty + lateVar + lateLet + lateFunction(), 21);

// Writes fail, whichever kind of built-in it is and whether or not the property was materialized before.
(function () {
    "use strict";
    let original = Array.prototype.map;
    shouldThrow(() => { Array.prototype.map = function () { }; }, TypeError);
    shouldThrow(() => { Array.prototype.fresh = 1; }, TypeError);
    shouldThrow(() => { delete Array.prototype.map; }, TypeError);
    shouldThrow(() => { Object.defineProperty(Array.prototype, "map", { value: 1 }); }, TypeError);
    shouldThrow(() => { Object.setPrototypeOf(Array.prototype, null); }, TypeError);
    shouldThrow(() => { Array.prototype.map.call = function () { }; }, TypeError);
    shouldThrow(() => { Array.prototype.flatMap.call = function () { }; }, TypeError);
    shouldThrow(() => { Object.defineProperty(Array.prototype.map, "name", { value: "other" }); }, TypeError);
    shouldThrow(() => { Array.from = 1; }, TypeError);
    shouldThrow(() => { Array.isArray.extra = 1; }, TypeError);
    shouldThrow(() => { Math.max = 1; }, TypeError);
    shouldThrow(() => { Math.min.extra = 1; }, TypeError);
    shouldThrow(() => { parseInt.extra = 1; }, TypeError);
    shouldThrow(() => { isNaN.extra = 1; }, TypeError);
    shouldThrow(() => { eval.extra = 1; }, TypeError);
    shouldThrow(() => { Object.getOwnPropertyDescriptor(Map.prototype, "size").get.extra = 1; }, TypeError);
    shouldThrow(() => { Object.getOwnPropertyDescriptor(Object.prototype, "__proto__").set.extra = 1; }, TypeError);
    shouldThrow(() => { Object.getOwnPropertyDescriptor(RegExp, "input").get.extra = 1; }, TypeError);
    shouldThrow(() => { Object.getOwnPropertyDescriptor(RegExp, "input").set.extra = 1; }, TypeError);
    shouldThrow(() => { Array.prototype[Symbol.unscopables].map = true; }, TypeError);
    shouldThrow(() => { Intl.Collator.prototype.compare = 1; }, TypeError);
    shouldThrow(() => { Intl.NumberFormat.supportedLocalesOf.extra = 1; }, TypeError);
    shouldThrow(() => { Temporal.Now.instant = 1; }, TypeError);
    shouldThrow(() => { Object.getPrototypeOf(Int8Array).from = 1; }, TypeError);
    shouldThrow(() => { Object.getPrototypeOf(Int8Array.prototype).map = 1; }, TypeError);
    shouldThrow(() => { Object.getPrototypeOf([][Symbol.iterator]()).next = 1; }, TypeError);
    shouldThrow(() => { Object.getPrototypeOf(function* () { }).prototype.next = 1; }, TypeError);
    shouldThrow(() => { Object.getPrototypeOf(async function () { }).constructor.extra = 1; }, TypeError);
    shouldBe(Array.prototype.map, original);
    shouldBe([1, 2, 3].map(x => x * 2).join(), "2,4,6");
    shouldBe(Array.prototype.map.call([1, 2], x => x + 1).join(), "2,3");
})();

// A function's lazy own properties still materialize.
shouldBe(Array.prototype.reduce.name, "reduce");
shouldBe(Array.prototype.reduce.length, 1);
shouldBe(Object.getOwnPropertyNames(Array.prototype.reduceRight).sort().join(), "length,name");

// Attributes stay as they are, so what inherits from a built-in can shadow its properties by assignment.
(function () {
    "use strict";
    shouldBe(Object.getOwnPropertyDescriptor(Object.prototype, "toString").writable, true);
    shouldBe(Object.isFrozen(Object.prototype), false);
    let object = { };
    object.toString = () => "mine";
    shouldBe(String(object), "mine");
    class List extends Array { }
    List.prototype.map = function () { return "mapped"; };
    shouldBe(new List().map(), "mapped");
    List.from = () => "from";
    shouldBe(List.from(), "from");
    function f() { }
    f.call = () => "called";
    shouldBe(f.call(), "called");
    let error = new TypeError("message");
    error.name = "Renamed";
    error.message = "changed";
    shouldBe(String(error), "Renamed: changed");
})();

// What a program creates is not a built-in.
shouldBe($vm.hasImmutableProperties(function () { }), false);
shouldBe($vm.hasImmutableProperties((function () { }).prototype), false);
shouldBe($vm.hasImmutableProperties(class { }), false);
shouldBe($vm.hasImmutableProperties((function () { }).bind()), false);
shouldBe($vm.hasImmutableProperties([]), false);
shouldBe($vm.hasImmutableProperties(new Map), false);
shouldBe($vm.hasImmutableProperties(new Function("")), false);

// A ShadowRealm's global object.
{
    let realm = new ShadowRealm;
    let attempt = source => realm.evaluate(`"use strict"; try { ${source}; "no error"; } catch (error) { error.name; }`);
    shouldBe(attempt("Array.prototype.map = 1"), "TypeError");
    shouldBe(attempt("Array.prototype.map.call = 1"), "TypeError");
    shouldBe(attempt("Math.max = 1"), "TypeError");
    shouldBe(attempt("new Intl.Segmenter().segment('a').__proto__.containing = 1"), "TypeError");
    shouldBe(attempt("globalThis.fresh = 1"), "no error");
}

// Built-ins that look at other built-ins before they take a fast path, each used for the first time here.
shouldBe(new Int8Array([3, 1, 2]).slice(1).join(), "1,2");
shouldBe(new Uint8Array(new ArrayBuffer(4).slice(1)).length, 3);
shouldBe([..."a,b".split(/,/), ..."a1b".replace(/\d/, "-")].join(""), "aba-b");
shouldBe([...new Set([1, 1, 2])].concat([...new Map([[3, 4]]).keys()]).join(), "1,2,3");
shouldBe(Object.entries(Object.fromEntries([["a", 1]])).join(), "a,1");
