// The own keys of a sloppy arguments object after an index was deleted, stored again or redefined.
// https://tc39.es/ecma262/#sec-ordinaryownpropertykeys lists the array indices first, in ascending order.
// An index that is not enumerable is in the lists that include such keys, and in no other list.

function direct(a, b, c) { return arguments; }
function scoped(a, b, c) { var capture = function () { return a + b + c; }; return arguments; }
function scopedWithOverflow(a) { var capture = function () { return a; }; return arguments; }
noInline(direct);
noInline(scoped);
noInline(scopedWithOverflow);

function keys(object) { return Object.keys(object); }
function entries(object) { return Object.entries(object); }
function values(object) { return Object.values(object); }
function names(object) { return Object.getOwnPropertyNames(object); }
function symbols(object) { return Object.getOwnPropertySymbols(object); }
function ownKeys(object) { return Reflect.ownKeys(object); }
function stringify(object) { return JSON.stringify(object); }
function forIn(object) {
    var result = [];
    for (var key in object)
        result.push(key);
    return result;
}
function forInThatShowsIndex2(object) {
    var result = [];
    for (var key in object) {
        if (key === "0")
            Object.defineProperty(object, 2, { enumerable: true });
        result.push(key);
    }
    return result;
}
noInline(keys);
noInline(entries);
noInline(values);
noInline(names);
noInline(symbols);
noInline(ownKeys);
noInline(stringify);
noInline(forIn);
noInline(forInThatShowsIndex2);

function shouldBeList(actual, expected, what, list) {
    var isEqual = actual.length === expected.length;
    for (var i = 0; isEqual && i < expected.length; ++i)
        isEqual = actual[i] === expected[i];
    if (!isEqual)
        throw new Error(what + ": " + list + ": expected [" + expected.map(String) + "] but got [" + actual.map(String) + "]");
}
noInline(shouldBeList);

// enumerable: the indices that are enumerable. all: every own index. Both in the order the lists must have.
function check(args, enumerable, all, what) {
    var enumerableKeys = [];
    var enumerableValues = [];
    var json = "";
    for (var i = 0; i < enumerable.length; ++i) {
        var value = args[enumerable[i]];
        enumerableKeys.push(String(enumerable[i]));
        enumerableValues.push(value);
        json += (i ? "," : "") + '"' + enumerable[i] + '":' + JSON.stringify(value);
    }
    var allNames = [];
    for (var i = 0; i < all.length; ++i)
        allNames.push(String(all[i]));
    allNames.push("length", "callee");

    shouldBeList(keys(args), enumerableKeys, what, "Object.keys");
    shouldBeList(forIn(args), enumerableKeys, what, "for-in");
    var entryKeys = [];
    var entryValues = [];
    for (var entry of entries(args)) {
        entryKeys.push(entry[0]);
        entryValues.push(entry[1]);
    }
    shouldBeList(entryKeys, enumerableKeys, what, "keys of Object.entries");
    shouldBeList(entryValues, enumerableValues, what, "values of Object.entries");
    shouldBeList(values(args), enumerableValues, what, "Object.values");
    shouldBeList([stringify(args)], ["{" + json + "}"], what, "JSON.stringify");

    shouldBeList(names(args), allNames, what, "Object.getOwnPropertyNames");
    allNames.push(Symbol.iterator);
    shouldBeList(ownKeys(args), allNames, what, "Reflect.ownKeys");
    shouldBeList(symbols(args), [Symbol.iterator], what, "Object.getOwnPropertySymbols");

    // Each list agrees with what the object says about each index.
    for (var i = 0; i < all.length; ++i) {
        var index = all[i];
        if (!Object.prototype.hasOwnProperty.call(args, index))
            throw new Error(what + ": index " + index + " is listed but is not an own property");
        if (Object.prototype.propertyIsEnumerable.call(args, index) !== enumerable.includes(index))
            throw new Error(what + ": index " + index + " has another enumerability than the lists say");
    }
}
noInline(check);

// Each scenario gets the arguments object of a call with the arguments 0, 1 and 2.
var scenarios = [
    function untouched(args, what) {
        check(args, [0, 1, 2], [0, 1, 2], what);
    },
    function hiddenFirst(args, what) {
        Object.defineProperty(args, 0, { enumerable: false });
        // V8 leaves "0" out of Object.getOwnPropertyNames() and Reflect.ownKeys() too. The specification does not.
        check(args, [1, 2], [0, 1, 2], what);
    },
    function hiddenMiddle(args, what) {
        Object.defineProperty(args, 1, { enumerable: false });
        check(args, [0, 2], [0, 1, 2], what);
    },
    function hiddenThenShown(args, what) {
        Object.defineProperty(args, 1, { enumerable: false });
        Object.defineProperty(args, 1, { enumerable: true });
        check(args, [0, 1, 2], [0, 1, 2], what);
    },
    function deleted(args, what) {
        delete args[1];
        check(args, [0, 2], [0, 2], what);
    },
    function deletedThenStored(args, what) {
        delete args[1];
        args[1] = "again";
        check(args, [0, 1, 2], [0, 1, 2], what);
    },
    function firstDeletedThenStored(args, what) {
        delete args[0];
        args[0] = "again";
        check(args, [0, 1, 2], [0, 1, 2], what);
    },
    function deletedThenDefinedHidden(args, what) {
        delete args[1];
        Object.defineProperty(args, 1, { value: "again", writable: true, enumerable: false, configurable: true });
        check(args, [0, 2], [0, 1, 2], what);
    },
    function getter(args, what) {
        Object.defineProperty(args, 1, { get: function () { return "got"; }, enumerable: true, configurable: true });
        check(args, [0, 1, 2], [0, 1, 2], what);
    },
    function hiddenGetter(args, what) {
        Object.defineProperty(args, 1, { get: function () { return "got"; }, enumerable: false, configurable: true });
        check(args, [0, 2], [0, 1, 2], what);
    },
    function readOnly(args, what) {
        Object.defineProperty(args, 0, { writable: false });
        check(args, [0, 1, 2], [0, 1, 2], what);
    },
    function storedPastTheCount(args, what) {
        args[5] = 5;
        args[3] = 3;
        check(args, [0, 1, 2, 3, 5], [0, 1, 2, 3, 5], what);
    },
    function storedPastTheCountAndStoredAgain(args, what) {
        delete args[0];
        args[4] = 4;
        args[0] = "again";
        check(args, [0, 1, 2, 4], [0, 1, 2, 4], what);
    },
    function storedPastTheCountStoredAgainAndHidden(args, what) {
        delete args[0];
        args[4] = 4;
        args[0] = "again";
        Object.defineProperty(args, 2, { enumerable: false });
        Object.defineProperty(args, 7, { value: 7, writable: true, enumerable: false, configurable: true });
        check(args, [0, 1, 4], [0, 1, 2, 4, 7], what);
    },
    function storedFarPastTheCount(args, what) {
        args[100000] = "far";
        delete args[1];
        args[1] = "again";
        check(args, [0, 1, 2, 100000], [0, 1, 2, 100000], what);
    },
    function sealedWithHidden(args, what) {
        Object.defineProperty(args, 1, { enumerable: false });
        Object.seal(args);
        check(args, [0, 2], [0, 1, 2], what);
    },
    function frozen(args, what) {
        Object.freeze(args);
        check(args, [0, 1, 2], [0, 1, 2], what);
    },
    function notExtensible(args, what) {
        Object.preventExtensions(args);
        check(args, [0, 1, 2], [0, 1, 2], what);
        delete args[0];
        check(args, [1, 2], [1, 2], what + " after delete");
    },
    function listedBetweenChanges(args, what) {
        check(args, [0, 1, 2], [0, 1, 2], what);
        delete args[1];
        args[1] = "again";
        check(args, [0, 1, 2], [0, 1, 2], what + " after store");
        Object.defineProperty(args, 0, { enumerable: false });
        check(args, [1, 2], [0, 1, 2], what + " after hide");
        delete args[0];
        check(args, [1, 2], [1, 2], what + " after delete");
    },
    function pushed(args, what) {
        Array.prototype.push.call(args, 3);
        check(args, [0, 1, 2, 3], [0, 1, 2, 3], what);
    },
    function unshifted(args, what) {
        Array.prototype.unshift.call(args, "first");
        check(args, [0, 1, 2, 3], [0, 1, 2, 3], what);
    },
    function popped(args, what) {
        Array.prototype.pop.call(args);
        check(args, [0, 1], [0, 1], what);
    },
    function shifted(args, what) {
        Array.prototype.shift.call(args);
        check(args, [0, 1], [0, 1], what);
    },
    function definedPastTheObject(args, what) {
        // Array.of() defines index 0 with putDirectIndex(), which does not ask defineOwnProperty() of the object.
        // So the indexed storage has a copy of an index that is still a mapped argument. Each list has it once.
        Array.of.call(function () { return args; }, "defined");
        args[4] = 4;
        check(args, [0, 1, 2, 4], [0, 1, 2, 4], what);
    },
    function shownDuringForIn(args, what) {
        Object.defineProperty(args, 2, { enumerable: false });
        // A for-in lists the keys when it starts, as it does for an array. Index 2 is not enumerable then.
        shouldBeList(forInThatShowsIndex2(args), ["0", "1"], what, "for-in that shows index 2");
        check(args, [0, 1, 2], [0, 1, 2], what + " after the loop");
    },
    function revived(args, what) {
        delete args[1];
        args[1] = "again";
        Object.defineProperty(args, 0, { enumerable: false });
        // JSON.parse() lists the keys of each object its walk comes to, and stores what the reviver returns.
        var seen = [];
        JSON.parse('{"first":0,"second":0}', function (key, value) {
            if (key === "first")
                this.second = args;
            if (this === args)
                seen.push(key);
            return value;
        });
        shouldBeList(seen, ["1", "2"], what, "keys that a reviver gets");
        check(args, [1, 2], [0, 1, 2], what);
    },
    function inherited(args, what) {
        delete args[1];
        args[1] = "again";
        Object.defineProperty(args, 0, { enumerable: false });
        var heir = Object.create(args);
        heir[2] = "own";
        heir.named = "own";
        shouldBeList(forIn(heir), ["2", "named", "1"], what, "for-in of an heir");
    },
    function behindProxy(args, what) {
        delete args[1];
        args[1] = "again";
        Object.defineProperty(args, 0, { enumerable: false });
        var proxy = new Proxy(args, {});
        shouldBeList(Object.keys(proxy), ["1", "2"], what, "Object.keys of a Proxy");
        shouldBeList(Reflect.ownKeys(proxy), ["0", "1", "2", "length", "callee", Symbol.iterator], what, "Reflect.ownKeys of a Proxy");
    },
];

// The function changes its own arguments object. A parameter and its index stay bound while only enumerable changes.
function directChangedInside(a, b, c) {
    Object.defineProperty(arguments, 0, { enumerable: false });
    delete arguments[1];
    arguments[1] = "again";
    a = "stored";
    return arguments;
}
function scopedChangedInside(a, b, c) {
    var capture = function () { return a + b + c; };
    Object.defineProperty(arguments, 0, { enumerable: false });
    delete arguments[1];
    arguments[1] = "again";
    a = "stored";
    return arguments;
}
noInline(directChangedInside);
noInline(scopedChangedInside);

function checkChangedInside(args, what) {
    if (args[0] !== "stored")
        throw new Error(what + ": index 0 is no longer bound to its parameter");
    check(args, [1, 2], [0, 1, 2], what);
}

// More keys than PropertyNameArrayBuilder::add() compares one by one.
function manyArguments(makeArguments, hidden, what) {
    var args = makeArguments(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23);
    delete args[3];
    args[3] = "again";
    delete args[22];
    args[22] = "again";
    Object.defineProperty(args, hidden, { enumerable: false });
    args[30] = 30;
    var all = [];
    for (var i = 0; i < 24; ++i)
        all.push(i);
    all.push(30);
    check(args, all.filter(function (index) { return index !== hidden; }), all, what);
}

// Only the first index of this ScopedArguments has a parameter. The object does not record a changed descriptor
// for the other indices, so only the indexed storage can say what became of one that is no longer mapped.
function overflow(what) {
    var args = scopedWithOverflow(0, 1, 2, 3, 4, 5);
    delete args[1];
    delete args[2];
    args[2] = "again";
    delete args[3];
    Object.defineProperty(args, 3, { value: "hidden", writable: true, enumerable: false, configurable: true });
    Object.defineProperty(args, 4, { get: function () { return "got"; }, enumerable: false, configurable: true });
    args[8] = 8;
    delete args[0];
    args[0] = "again";
    check(args, [0, 2, 5, 8], [0, 2, 3, 4, 5, 8], what);
}

function extra(turn, storage) {
    switch (turn % 5) {
    case 0:
        checkChangedInside(directChangedInside(0, 1, 2), "DirectArguments changed inside" + storage);
        break;
    case 1:
        checkChangedInside(scopedChangedInside(0, 1, 2), "ScopedArguments changed inside" + storage);
        break;
    case 2:
        manyArguments(direct, 21, "DirectArguments with many arguments" + storage);
        break;
    case 3:
        // The hidden index has a parameter. See overflow() for the other indices.
        manyArguments(scoped, 2, "ScopedArguments with many arguments" + storage);
        break;
    case 4:
        overflow("ScopedArguments with arguments past the parameters" + storage);
        break;
    }
}

// Every turn of the loop makes the objects, so that each tier makes them. The lists are checked on one turn in `stride`,
// which is often enough for each scenario on each class.
function run() {
    var turns = Math.max(100, 2 * scenarios.length);
    var loopCount = Math.max(testLoopCount, 2 * scenarios.length);
    var stride = Math.max(1, Math.floor(loopCount / turns));
    for (var i = 0; i < loopCount; ++i) {
        var directArgs = direct(0, 1, 2);
        var scopedArgs = scoped(0, 1, 2);
        if (i % stride)
            continue;

        var turn = i / stride;
        var scenario = scenarios[(turn >> 1) % scenarios.length];
        if (turn & 1)
            scenario(scopedArgs, "ScopedArguments " + scenario.name);
        else
            scenario(directArgs, "DirectArguments " + scenario.name);
        if (turn % 6 === 5)
            extra((turn / 6) | 0, "");
    }
}

// Once more with the other kind of storage these objects can have: an indexed accessor on Object.prototype makes
// every object of the realm use SlowPutArrayStorage.
function runWithSlowPutStorage() {
    Object.defineProperty(Object.prototype, 1000, { get: function () { }, set: function (value) { }, configurable: true });
    for (var i = 0; i < scenarios.length; ++i) {
        if (i & 1)
            scenarios[i](scoped(0, 1, 2), "ScopedArguments with slow put storage " + scenarios[i].name);
        else
            scenarios[i](direct(0, 1, 2), "DirectArguments with slow put storage " + scenarios[i].name);
    }
    for (var i = 0; i < 5; ++i)
        extra(i, " with slow put storage");
}

// Only the functions that make the objects and the ones that list their keys have to run in every tier.
for (var helper of [shouldBeList, check, checkChangedInside, manyArguments, overflow, extra, run, runWithSlowPutStorage].concat(scenarios))
    noDFG(helper);

run();
runWithSlowPutStorage();
