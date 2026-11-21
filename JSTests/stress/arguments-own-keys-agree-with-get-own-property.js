// Each program makes a sloppy arguments object and changes it a few times at random. After each change, every list
// of its own keys has to agree with what the object says about each index: the indices come first, in ascending
// order (https://tc39.es/ecma262/#sec-ordinaryownpropertykeys), and a list of enumerable keys has the enumerable ones.
// A failure names the program and the changes, so that it can be run again.

function direct(a, b, c) { return arguments; }
function scoped(a, b, c) { var capture = function () { return a + b; }; return arguments; }
function scopedWithOneParameter(a) { var capture = function () { return a; }; return arguments; }
noInline(direct);
noInline(scoped);
noInline(scopedWithOneParameter);

var makers = [
    function () { return direct(10, 11, 12); },
    function () { return direct(10); },
    function () { return direct(10, 11, 12, 13, 14); },
    function () { return scoped(10, 11, 12); },
    function () { return scoped(10); },
    function () { return scoped(10, 11, 12, 13, 14); },
    function () { return scopedWithOneParameter(10, 11, 12, 13); },
];

function randomNumbers(seed) {
    return function () {
        seed = seed + 0x6D2B79F5 | 0;
        var t = Math.imul(seed ^ seed >>> 15, 1 | seed);
        t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t;
        return ((t ^ t >>> 14) >>> 0) / 4294967296;
    };
}

function keys(object) { return Object.keys(object); }
function entries(object) { return Object.entries(object); }
function names(object) { return Object.getOwnPropertyNames(object); }
function ownKeys(object) { return Reflect.ownKeys(object); }
function forIn(object) {
    var result = [];
    for (var key in object)
        result.push(key);
    return result;
}
noInline(keys);
noInline(entries);
noInline(names);
noInline(ownKeys);
noInline(forIn);

var isIndex = /^[0-9]+$/;
var jsonKey = /"([^"]+)":/g;

function check(args, trace) {
    var all = [];
    var enumerable = [];
    for (var index = 0; index < 10; ++index) {
        if (!Object.prototype.hasOwnProperty.call(args, index))
            continue;
        all.push(String(index));
        if (Object.prototype.propertyIsEnumerable.call(args, index))
            enumerable.push(String(index));
    }

    // The keys that are not indices are taken in the order of Reflect.ownKeys(). This test is about the indices.
    var strings = [];
    for (var key of ownKeys(args)) {
        if (typeof key !== "string")
            continue;
        strings.push(key);
        if (isIndex.test(key))
            continue;
        all.push(key);
        if (Object.prototype.propertyIsEnumerable.call(args, key))
            enumerable.push(key);
    }

    var entryKeys = [];
    for (var entry of entries(args))
        entryKeys.push(entry[0]);
    // JSON.stringify() leaves out a key whose value is undefined.
    var jsonKeys = [];
    for (var match of JSON.stringify(args).matchAll(jsonKey))
        jsonKeys.push(match[1]);
    var enumerableWithValue = [];
    for (var key of enumerable) {
        if (args[key] !== undefined)
            enumerableWithValue.push(key);
    }

    var lists = [
        ["Reflect.ownKeys", strings, all],
        ["Object.getOwnPropertyNames", names(args), all],
        ["Object.keys", keys(args), enumerable],
        ["for-in", forIn(args), enumerable],
        ["Object.entries", entryKeys, enumerable],
        ["JSON.stringify", jsonKeys, enumerableWithValue],
    ];
    for (var list of lists) {
        if (String(list[1]) !== String(list[2]))
            throw new Error(list[0] + " gives [" + list[1] + "] and the descriptors say [" + list[2] + "] after " + trace.join("; "));
    }
}

function change(args, random, trace) {
    var index = (random() * 8) | 0;
    var isEnumerable = random() < 0.5;
    try {
        switch ((random() * 9) | 0) {
        case 0:
            trace.push("delete args[" + index + "]");
            delete args[index];
            break;
        case 1:
            trace.push("args[" + index + "] = 1");
            args[index] = 1;
            break;
        case 2:
            trace.push("define " + index + " { enumerable: false }");
            Object.defineProperty(args, index, { enumerable: false });
            break;
        case 3:
            trace.push("define " + index + " { enumerable: true }");
            Object.defineProperty(args, index, { enumerable: true });
            break;
        case 4:
            trace.push("define " + index + " { value, enumerable: " + isEnumerable + " }");
            Object.defineProperty(args, index, { value: 2, writable: true, enumerable: isEnumerable, configurable: true });
            break;
        case 5:
            trace.push("define " + index + " { get, enumerable: " + isEnumerable + " }");
            Object.defineProperty(args, index, { get: function () { return 3; }, enumerable: isEnumerable, configurable: true });
            break;
        case 6:
            trace.push("define " + index + " { writable: false }");
            Object.defineProperty(args, index, { writable: false });
            break;
        case 7:
            trace.push("args.named" + index + " = 1");
            args["named" + index] = 1;
            break;
        case 8:
            if (index < 2) {
                trace.push("preventExtensions");
                Object.preventExtensions(args);
            } else if (index < 4) {
                trace.push("args.length = 2");
                args.length = 2;
            }
            break;
        }
    } catch (error) {
        // A change that the object refuses leaves it as it was.
        if (!(error instanceof TypeError))
            throw error;
        trace.push("(TypeError)");
    }
}

function run() {
    var programs = Math.min(testLoopCount, 50);
    for (var program = 0; program < programs; ++program) {
        var random = randomNumbers(program + 1);
        var args = makers[program % makers.length]();
        var trace = ["program " + program + ", maker " + (program % makers.length)];
        check(args, trace);
        for (var step = 0; step < 6; ++step) {
            change(args, random, trace);
            check(args, trace);
        }
    }
}

// Only the functions that make the objects and the ones that list their keys have to run in every tier.
noDFG(check);
noDFG(change);
noDFG(run);

run();
