// The classes with write hooks of their own that can be locked: arrays, functions, errors, RegExp objects, String objects and
// unmapped arguments objects. Their lazily materialized properties still appear after the lock.

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

function snapshot(object) {
    return JSON.stringify(Reflect.ownKeys(object).map(key => {
        let d = Object.getOwnPropertyDescriptor(object, key);
        return [String(key), "value" in d ? (typeof d.value === "object" || typeof d.value === "function" ? typeof d.value : String(d.value)) : "accessor", d.writable, d.enumerable, d.configurable];
    })) + " length:" + String(object.length);
}

// Arrays: every storage kind against every mutator, each mutator made hot on unlocked arrays of the same kind first.
{
    let kinds = {
        int32: () => [1, 2, 3, 4],
        double: () => [1.5, 2.5, 3.5],
        contiguous: () => ["a", {}, "c"],
        holey: () => { let a = [1, 2, 3]; a[40] = 9; return a; },
        arrayStorage: () => { let a = [1, 2, 3]; a[20000] = 1; delete a[20000]; a.length = 3; return a; },
        sparse: () => { let a = []; a[30000] = 1; a[0] = 7; return a; },
        undecided: () => new Array(4),
        copyOnWrite: () => [10, 20, 30],
        usedAsPrototype: () => { let a = [1, 2, 3]; Object.create(a); return a; },
        arrayLike: () => ({ 0: "a", 1: "b", length: 2 }),
    };
    let mutators = {
        push: a => Array.prototype.push.call(a, "X"),
        pop: a => Array.prototype.pop.call(a),
        shift: a => Array.prototype.shift.call(a),
        unshift: a => Array.prototype.unshift.call(a, "X", "X"),
        splice: a => Array.prototype.splice.call(a, 0, 1, "X", "X"),
        sort: a => Array.prototype.sort.call(a, () => -1),
        reverse: a => Array.prototype.reverse.call(a),
        fill: a => Array.prototype.fill.call(a, "X"),
        copyWithin: a => Array.prototype.copyWithin.call(a, 0, 1),
        lengthToZero: a => { a.length = 0; },
        lengthGrows: a => { a.length = 99; },
        storeFirst: a => { a[0] = "X"; },
        storeAtLength: a => { a[a.length] = "X"; },
        storeFar: a => { a[40000] = "X"; },
        deleteFirst: a => { delete a[0]; },
        defineFirst: a => Object.defineProperty(a, 0, { value: "X" }),
        defineLength: a => Object.defineProperty(a, "length", { value: 0 }),
        reflectSet: a => Reflect.set(a, 0, "X"),
        assign: a => Object.assign(a, ["X", "X"]),
        freeze: a => Object.freeze(a),
        seal: a => Object.seal(a),
    };
    for (let [kindName, make] of Object.entries(kinds)) {
        for (let [mutatorName, mutate] of Object.entries(mutators)) {
            for (let i = 0; i < 200; i++) {
                try {
                    mutate(make());
                } catch { }
            }
            let array = $vm.lockObject(make());
            let before = snapshot(array);
            for (let i = 0; i < 40; i++) {
                try {
                    mutate(array);
                } catch { }
            }
            shouldBe(snapshot(array), before, kindName + " / " + mutatorName);
        }
    }

    let array = $vm.lockObject([1, 2, 3]);
    shouldThrow(() => array.push(4), TypeError);
    shouldThrow(() => array.pop(), TypeError);
    shouldThrow(() => { "use strict"; array[0] = 9; }, TypeError);
    shouldThrow(() => { "use strict"; array.length = 0; }, TypeError);
    shouldBe(Reflect.defineProperty(array, "length", { value: 3 }), true);
    shouldBe(Reflect.defineProperty(array, 0, { value: 1 }), true);
    shouldBe(Reflect.defineProperty(array, 0, { value: 2 }), false);
    shouldBe(Reflect.deleteProperty(array, 7), true);
    shouldBe(Reflect.deleteProperty(array, 0), false);
    shouldBe(array.map(x => x * 2).join(), "2,4,6");
    shouldBe([...array].join(), "1,2,3");
    shouldBe($vm.isLockedObject(array.slice()), false);
}

// Functions: name, length and prototype are materialized lazily and still appear on a locked function.
{
    function declared(a, b) { }
    let arrow = (a) => { };
    let bound = declared.bind(null, 1);
    let native = Math.max;
    class Klass { static s() { } }
    for (let fn of [declared, arrow, bound, native, Klass])
        $vm.lockObject(fn);

    shouldBe(declared.name, "declared");
    shouldBe(declared.length, 2);
    shouldBe(typeof declared.prototype, "object");
    shouldBe(declared.prototype.constructor, declared);
    shouldBe(new declared() instanceof declared, true);
    shouldBe(arrow.name, "arrow");
    shouldBe(arrow.length, 1);
    shouldBe(bound.name, "bound declared");
    shouldBe(bound.length, 1);
    shouldBe(native.name, "max");
    shouldBe(native.length, 2);
    shouldBe(Klass.name, "Klass");
    shouldBe(Object.getOwnPropertyNames(declared).sort().join(), "length,name,prototype");

    for (let fn of [declared, arrow, bound, native, Klass]) {
        shouldThrow(() => { "use strict"; fn.extra = 1; }, TypeError);
        shouldThrow(() => { "use strict"; fn.name = "x"; }, TypeError);
        shouldBe(Reflect.defineProperty(fn, "name", { value: "x" }), false);
        shouldBe(Reflect.defineProperty(fn, "name", { value: fn.name }), true);
        shouldBe(Reflect.deleteProperty(fn, "name"), false);
        shouldBe(Reflect.deleteProperty(fn, "missing"), true);
        shouldBe(Reflect.setPrototypeOf(fn, null), false);
    }
    shouldThrow(() => { "use strict"; declared.prototype = {}; }, TypeError);
    shouldBe(declared.prototype.constructor, declared);
    // The prototype object itself was not locked.
    declared.prototype.method = function () { return 1; };
    shouldBe(new declared().method(), 1);
}

// Errors: line, column, sourceURL and stack are materialized lazily and still appear on a locked error.
{
    let error = $vm.lockObject(new RangeError("message"));
    shouldBe(typeof error.stack, "string");
    shouldBe(typeof error.line, "number");
    shouldBe(error.message, "message");
    shouldThrow(() => { "use strict"; error.message = "changed"; }, TypeError);
    shouldThrow(() => { "use strict"; error.stack = "changed"; }, TypeError);
    shouldThrow(() => { "use strict"; error.extra = 1; }, TypeError);
    shouldBe(Reflect.defineProperty(error, "stack", { value: "changed" }), false);
    shouldBe(Reflect.deleteProperty(error, "stack"), false);
    shouldBe(Reflect.deleteProperty(error, "message"), false);
    shouldBe(error.message, "message");
    shouldBe(typeof error.stack, "string");
}

// RegExp objects: lastIndex becomes non-writable (compiled code tests the object's own flag), so matching that has to update
// it fails as it does for a frozen RegExp; matching that does not need to update it works.
{
    let plain = $vm.lockObject(/b/);
    shouldBe(plain.test("abc"), true);
    shouldBe("abc".replace(plain, "X"), "aXc");
    shouldBe(Object.getOwnPropertyDescriptor(plain, "lastIndex").writable, false);
    shouldThrow(() => { "use strict"; plain.lastIndex = 2; }, TypeError);
    shouldThrow(() => { "use strict"; plain.extra = 1; }, TypeError);
    shouldThrow(() => plain.compile("c"), TypeError);
    shouldBe(plain.source, "b");

    let global = $vm.lockObject(/b/g);
    shouldThrow(() => global.exec("abcb"), TypeError);
    shouldBe(global.lastIndex, 0);
}

// String objects.
{
    let string = $vm.lockObject(new String("ab"));
    shouldBe(string.length, 2);
    shouldBe(string[1], "b");
    shouldThrow(() => { "use strict"; string.extra = 1; }, TypeError);
    shouldThrow(() => { "use strict"; string[5] = "c"; }, TypeError);
    shouldBe(Reflect.defineProperty(string, 5, { value: "c" }), false);
    shouldBe(Reflect.defineProperty(string, 0, { value: "a" }), true);
    shouldBe(Reflect.deleteProperty(string, 0), false);
    shouldBe(Reflect.deleteProperty(string, 5), true);
    shouldBe(String(string), "ab");
}

// Unmapped (strict-mode) arguments objects: callee, Symbol.iterator and length still appear.
{
    let args = (function () { "use strict"; return arguments; })(1, 2, 3);
    $vm.lockObject(args);
    shouldBe(args.length, 3);
    shouldBe([...args].join(), "1,2,3");
    shouldBe(typeof args[Symbol.iterator], "function");
    shouldThrow(() => args.callee, TypeError);
    shouldThrow(() => { "use strict"; args[0] = 9; }, TypeError);
    shouldThrow(() => { "use strict"; args.length = 0; }, TypeError);
    shouldThrow(() => { "use strict"; args.extra = 1; }, TypeError);
    shouldBe(Reflect.deleteProperty(args, 0), false);
    shouldBe(args[0], 1);
}

// Built-in prototypes and constructors: static properties are reified lazily and still appear after the lock.
{
    for (let object of [Array.prototype, Array, Math, JSON, Promise.prototype, Promise, RegExp.prototype, Map.prototype, Object])
        $vm.lockObject(object);
    shouldBe([3, 1, 2].toSorted().join(), "1,2,3");
    shouldBe(typeof Array.prototype.findLast, "function");
    shouldBe(Math.hypot(3, 4), 5);
    shouldBe(JSON.stringify([1]), "[1]");
    shouldBe(typeof Promise.allSettled, "function");
    shouldBe(Object.getOwnPropertyNames(Math).includes("fround"), true);
    shouldThrow(() => { "use strict"; Array.prototype.push = function () { }; }, TypeError);
    shouldThrow(() => { "use strict"; Array.prototype[0] = 1; }, TypeError);
    shouldThrow(() => { "use strict"; Math.max = function () { }; }, TypeError);
    shouldThrow(() => { "use strict"; Promise.prototype.then = function () { }; }, TypeError);
    shouldBe(Reflect.defineProperty(Array.prototype, Symbol.iterator, { value: function () { } }), false);
    shouldBe([1, 2, 3].map(x => x + 1).join(), "2,3,4");
    shouldBe([...new Map([[1, 2]])].join(), "1,2");
    let array = [];
    array.push(1);
    array[5] = 2;
    shouldBe(array.length, 6);
    shouldBe(array[3], undefined);
}
