//@ requireOptions("--immutableGlobals=*", "--useTemporal=1", "--useShadowRealm=1", "--exposeProfilersOnGlobalObject=1")

// --immutableGlobals=* makes every built-in immutable (JSObject::makePropertiesImmutable()) in each new global object: the global
// object, constructors, prototypes, namespace objects and functions, JavaScriptCore's and the embedder's (here the jsc shell), with
// or without a global name, whenever they are created. (This script cannot declare anything at the top level.)

(function () {
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

    const isImmutable = $vm.hasImmutableProperties;

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
        (() => { try { return $vm.ffiCallback({ args: [], returns: "void" }, () => { }); } catch { return null; } })(),
    ]`;

    // Follows every own property (value, getter and setter, and what the getter returns if that is the same object each time) and
    // every prototype from a global object.
    function mutableObjectsReachableFrom(global) {
        let seen = new Set([global.$vm]);
        let queue = [];
        let mutable = [];
        function visit(value, path) {
            if ((typeof value !== "object" && typeof value !== "function") || value === null || seen.has(value))
                return;
            seen.add(value);
            queue.push([value, path]);
        }
        visit(global, "globalThis");
        global.eval(sourceOfValues).forEach((value, index) => {
            if (value !== null)
                visit(Object.getPrototypeOf(value), "value " + index + ".__proto__");
        });
        while (queue.length) {
            let [object, path] = queue.pop();
            if (!isImmutable(object))
                mutable.push(path);
            if (typeof object === "function" && Object.hasOwn(object, "prototype") && object.prototype.constructor !== object)
                mutable.push(path + ".prototype.constructor is not " + path);
            visit(Object.getPrototypeOf(object), path + ".__proto__");
            for (let key of Reflect.ownKeys(object)) {
                let descriptor = Reflect.getOwnPropertyDescriptor(object, key);
                let pathOfKey = path + "." + String(key);
                visit(descriptor.value, pathOfKey);
                visit(descriptor.get, "get " + pathOfKey);
                visit(descriptor.set, "set " + pathOfKey);
                if (descriptor.get) {
                    try {
                        let result = Reflect.get(object, key);
                        if (result === Reflect.get(object, key))
                            visit(result, pathOfKey);
                    } catch { }
                }
            }
        }
        shouldBe(seen.size > 800, true);
        return mutable.join();
    }

    shouldBe(mutableObjectsReachableFrom($vm.createGlobalObject()), "");

    // The embedder's global object. What it holds is covered; from there on an object that is not a function is not followed.
    shouldBe(mutableObjectsReachableFrom(globalThis), "");
    shouldBe(mutableObjectsReachableFrom(createGlobalObject()), "");
    shouldBe(isImmutable($), true);
    shouldBe(isImmutable($.evalScript), true);
    shouldBe(isImmutable($.agent), false);

    (function () {
        "use strict";

        // The global object: what a name means does not change.
        shouldThrow(() => { globalThis.Array = 1; }, TypeError);
        shouldThrow(() => { globalThis.Date = 1; }, TypeError);
        shouldThrow(() => { globalThis.print = 1; }, TypeError);
        shouldThrow(() => { globalThis.fresh = 1; }, TypeError);
        shouldThrow(() => { delete globalThis.Math; }, TypeError);
        (0, eval)("undeclared = 1;");
        shouldBe("undeclared" in globalThis, false);
        for (let source of ["var a;", "function a() { }"])
            shouldThrow(() => loadString(source), TypeError);
        for (let source of ["let a;", "const a = 1;", "class a { }", "let Array;", "var b; let a;", "function b() { } let a;"])
            shouldThrow(() => loadString(source), SyntaxError);
        shouldBe(typeof a, "undefined");
        shouldBe(typeof b, "undefined");
        shouldBe(loadString("{ let a = 1; a + eval('let b = 2; b') + new Function('let c = 3; return c;')(); }"), 6);

        // Writes fail, whichever kind of built-in it is and whether or not the property was materialized before.
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
        shouldThrow(() => { Math.max = 1; }, TypeError);
        shouldThrow(() => { Math.min.extra = 1; }, TypeError);
        shouldThrow(() => { parseInt.extra = 1; }, TypeError);
        shouldThrow(() => { print.extra = 1; }, TypeError);
        shouldThrow(() => { performance.now = 1; }, TypeError);
        shouldThrow(() => { Object.getOwnPropertyDescriptor(Map.prototype, "size").get.extra = 1; }, TypeError);
        shouldThrow(() => { Object.getOwnPropertyDescriptor(RegExp, "input").set.extra = 1; }, TypeError);
        shouldThrow(() => { Array.prototype[Symbol.unscopables].map = true; }, TypeError);
        shouldThrow(() => { Intl.Collator.prototype.compare = 1; }, TypeError);
        shouldThrow(() => { Temporal.Now.instant = 1; }, TypeError);
        shouldThrow(() => { Object.getPrototypeOf(Int8Array).from = 1; }, TypeError);
        shouldThrow(() => { Object.getPrototypeOf([][Symbol.iterator]()).next = 1; }, TypeError);
        shouldThrow(() => { Object.getPrototypeOf(function* () { }).prototype.next = 1; }, TypeError);
        shouldBe(Array.prototype.map, original);
        shouldBe([1, 2, 3].map(x => x * 2).join(), "2,4,6");

        // A function's lazy own properties still materialize.
        shouldBe(Array.prototype.reduce.name, "reduce");
        shouldBe(Object.getOwnPropertyNames(Array.prototype.reduceRight).sort().join(), "length,name");

        // Attributes stay as they are, so what inherits from a built-in can shadow its properties by assignment.
        shouldBe(Object.getOwnPropertyDescriptor(Object.prototype, "toString").writable, true);
        shouldBe(Object.isFrozen(Object.prototype), false);
        let object = { };
        object.toString = () => "mine";
        shouldBe(String(object), "mine");
        class List extends Array { }
        List.prototype.map = function () { return "mapped"; };
        shouldBe(new List().map(), "mapped");
        let error = new TypeError("message");
        error.name = "Renamed";
        shouldBe(String(error), "Renamed: message");
    })();

    // What a program creates is not a built-in, whatever it makes a prototype of.
    class Base { }
    class Derived extends Base { }
    let prototype = { };
    for (let object of [function () { }, (function () { }).prototype, Base, Base.prototype, Derived.prototype, (function () { }).bind(), [], new Map,
        new Function(""), prototype, Object.create(prototype), Function.prototype.bind.call(Object.setPrototypeOf(function () { }, prototype)), new Derived])
        shouldBe(isImmutable(object), false);
    shouldBe(isImmutable(prototype), false);

    // A ShadowRealm's global object.
    {
        let realm = new ShadowRealm;
        let attempt = source => realm.evaluate(`"use strict"; try { ${source}; "no error"; } catch (error) { error.name; }`);
        shouldBe(attempt("Array.prototype.map = 1"), "TypeError");
        shouldBe(attempt("Array.prototype.map.call = 1"), "TypeError");
        shouldBe(attempt("new Intl.Segmenter().segment('a').__proto__.containing = 1"), "TypeError");
        shouldBe(attempt("globalThis.fresh = 1"), "TypeError");
    }

    // Built-ins that look at other built-ins before they take a fast path, each used for the first time here.
    shouldBe(new Int8Array([3, 1, 2]).slice(1).join(), "1,2");
    shouldBe(new Uint8Array(new ArrayBuffer(4).slice(1)).length, 3);
    shouldBe([..."a,b".split(/,/), ..."a1b".replace(/\d/, "-")].join(""), "aba-b");
    shouldBe([...new Set([1, 1, 2])].concat([...new Map([[3, 4]]).keys()]).join(), "1,2,3");
    shouldBe(Object.entries(Object.fromEntries([["a", 1]])).join(), "a,1");
})();
