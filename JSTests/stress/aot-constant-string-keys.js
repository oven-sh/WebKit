//@ runDefault("--compileMainScriptAheadOfTime=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function nameOfError(f) {
    try {
        f();
    } catch (error) {
        return error.constructor.name;
    }
    return "nothing was thrown";
}

function readsName(o) { const key = "name"; return o[key]; }
function readsLength(o) { const key = "length"; return o[key]; }
function readsProto(o) { const key = "__proto__"; return o[key]; }
function readsToFixed(o) { const key = "toFixed"; return o[key]; }
function writesName(o, v) { const key = "name"; o[key] = v; }
function writesNameStrictly(o, v) { "use strict"; const key = "name"; o[key] = v; }
function writesLength(o, v) { const key = "length"; o[key] = v; }
function writesProto(o, v) { const key = "__proto__"; o[key] = v; }
function updatesName(o) { const key = "name"; o[key] += "!"; return o[key]; }
function readsAndWritesName(o) { const key = "name"; o[key] = o[key] + "!"; return o[key]; }
function readsProperty(o) { return o.name; }
function increments(o) { const key = "count"; o[key]++; return o[key]; }
function preincrements(o) { const key = "count"; return ++o[key]; }
function multiplies(o) { const key = "count"; o[key] *= 2; return o[key]; }
function assignsIfMissing(o) { const key = "count"; o[key] ??= 5; return o[key]; }
function assignsIfFalsy(o) { const key = "count"; o[key] ||= 6; return o[key]; }
function assignsIfTruthy(o) { const key = "count"; o[key] &&= 7; return o[key]; }
function updatesZero(o) { const key = "0"; o[key] += 1; return o[key]; }
function updatesFraction(o) { const key = "1.5"; o[key] += 1; return o[key]; }
function updatesAnyKey(o, key) { o[key] += 1; return o[key]; }

function readsZero(o) { const key = "0"; return o[key]; }
function readsTwelve(o) { const key = "12"; return o[key]; }
function readsLargestIndex(o) { const key = "4294967294"; return o[key]; }
function readsBeyondLargestIndex(o) { const key = "4294967295"; return o[key]; }
function readsMinusZero(o) { const key = "-0"; return o[key]; }
function readsMinusOne(o) { const key = "-1"; return o[key]; }
function readsFraction(o) { const key = "1.5"; return o[key]; }
function readsNaN(o) { const key = "NaN"; return o[key]; }
function readsInfinity(o) { const key = "Infinity"; return o[key]; }
function readsLeadingZero(o) { const key = "01"; return o[key]; }
function writesZero(o, v) { const key = "0"; o[key] = v; }
function writesFraction(o, v) { const key = "1.5"; o[key] = v; }
function readsUnusualKey(o) { const key = "no identifier of this program"; return o[key]; }
function readsSymbol(o) { const key = Symbol.iterator; return o[key]; }
function readsAnyKey(o, key) { return o[key]; }
function writesAnyKey(o, key, v) { o[key] = v; }
function readsNumber(o) { const key = 1; return o[key]; }

for (let round = 0; round < 3; round++) {
    check(readsName({ name: "own" }), "own", "an own property");
    check(readsName({ other: 1, name: "second" }), "second", "an own property of another shape");
    check(readsName(Object.create({ name: "inherited" })), "inherited", "an inherited property");
    check(readsName({}), undefined, "a missing property");
    check(readsName(function named() { }), "named", "the name of a function");
    check(readsName("text"), undefined, "a missing property of a string");
    check(readsName(5), undefined, "a missing property of a number");
    check(readsName(Symbol("s")), undefined, "a missing property of a symbol");
    check(nameOfError(() => readsName(undefined)), "TypeError", "a property of undefined");
    check(nameOfError(() => readsName(null)), "TypeError", "a property of null");

    let receivers = [];
    let withAccessor = {
        get name() { receivers.push(this); return "from getter"; },
        set name(v) { receivers.push(this, v); },
    };
    check(readsName(withAccessor), "from getter", "a getter");
    check(receivers.length === 1 && receivers[0], withAccessor, "the receiver of the getter");
    let heir = Object.create(withAccessor);
    check(readsName(heir), "from getter", "an inherited getter");
    check(receivers[1], heir, "the receiver of the inherited getter");
    writesName(heir, "sent");
    check(receivers[2], heir, "the receiver of the inherited setter");
    check(receivers[3], "sent", "the value passed to the setter");
    check(Object.hasOwn(heir, "name"), false, "a setter adds no own property");
    check(nameOfError(() => readsName({ get name() { throw new RangeError("getter"); } })), "RangeError", "a getter that throws");
    check(nameOfError(() => writesName({ set name(v) { throw new RangeError("setter"); } }, 1)), "RangeError", "a setter that throws");

    let seen = [];
    let proxy = new Proxy({ name: "target" }, {
        get(target, key, receiver) { seen.push("get", typeof key, key); return Reflect.get(target, key, receiver); },
        set(target, key, value, receiver) { seen.push("set", typeof key, key, value); return Reflect.set(target, key, value, receiver); },
    });
    check(readsName(proxy), "target", "a proxy");
    writesName(proxy, "changed");
    check(readsName(proxy), "changed", "a proxy after a write");
    check(seen.join(), "get,string,name,set,string,name,changed,get,string,name", "what the traps saw");

    check(readsLength([1, 2, 3]), 3, "the length of an array");
    check(readsLength("four"), 4, "the length of a string");
    check(readsLength(new Uint8Array(5)), 5, "the length of a typed array");
    check(readsLength({ length: "own" }), "own", "an ordinary property called length");
    check(readsLength(function (a, b) { }), 2, "the length of a function");
    check(readsProto({}), ({}).__proto__, "__proto__ of an object");
    check(readsProto([]), Array.prototype, "__proto__ of an array");
    check(readsProto(Object.create(null)), undefined, "__proto__ without Object.prototype");
    check(readsProto(1), Number.prototype, "__proto__ of a number");
    check(readsToFixed(1.5), Number.prototype.toFixed, "a method of a number");

    let plain = {};
    writesName(plain, 1);
    check(plain.name, 1, "a property that is added");
    writesName(plain, 2);
    check(plain.name, 2, "a property that is replaced");
    check(Object.keys(plain).join(), "name", "the keys afterwards");
    let frozen = Object.freeze({ name: "frozen" });
    writesName(frozen, "ignored");
    check(frozen.name, "frozen", "a write to a frozen object in sloppy mode");
    check(nameOfError(() => writesNameStrictly(frozen, "refused")), "TypeError", "a write to a frozen object in strict mode");
    writesName("text", "ignored");
    check(nameOfError(() => writesNameStrictly("text", "refused")), "TypeError", "a write to a string in strict mode");
    check(nameOfError(() => writesName(undefined, 1)), "TypeError", "a write to undefined");
    check(nameOfError(() => writesNameStrictly(null, 1)), "TypeError", "a write to null");
    let strict = {};
    writesNameStrictly(strict, "stored");
    check(strict.name, "stored", "a write in strict mode");
    check(nameOfError(() => writesNameStrictly({ get name() { return 1; } }, 2)), "TypeError", "a write to a property with only a getter in strict mode");
    let array = [1, 2, 3, 4];
    writesLength(array, 2);
    check(array.join(), "1,2", "the length of an array is written");
    check(nameOfError(() => writesLength(array, -1)), "RangeError", "an invalid length");
    let child = {}, parent = { inherited: true };
    writesProto(child, parent);
    check(Object.getPrototypeOf(child), parent, "__proto__ is written");
    check(Object.hasOwn(child, "__proto__"), false, "it adds no own property");
    check(updatesName({ name: "x" }), "x!", "a compound assignment");
    check(readsAndWritesName({ name: "x" }), "x!", "a read and a write");
    check(readsProperty({ name: "x" }), "x", "a read by name");
    check(increments({ count: 1 }), 2, "an increment");
    check(preincrements({ count: 1 }), 2, "an increment whose result is used");
    check(multiplies({ count: 4 }), 8, "a multiplication");
    check(increments({}), NaN, "an increment of a missing property");
    check(assignsIfMissing({}), 5, "an assignment to a missing property");
    check(assignsIfMissing({ count: 0 }), 0, "no assignment to a property that is there");
    check(assignsIfFalsy({ count: 0 }), 6, "an assignment to a falsy property");
    check(assignsIfFalsy({ count: 1 }), 1, "no assignment to a truthy property");
    check(assignsIfTruthy({ count: 1 }), 7, "an assignment to a truthy property");
    check(assignsIfTruthy({ count: 0 }), 0, "no assignment to a falsy property");
    check(nameOfError(() => increments(undefined)), "TypeError", "an increment of a property of undefined");
    check(nameOfError(() => assignsIfMissing(null)), "TypeError", "an assignment to a property of null");
    {
        let order = [];
        let counted = { get count() { order.push("get"); return 1; }, set count(v) { order.push("set " + v); } };
        increments(counted);
        check(order.join(), "get,set 2,get", "an increment calls the getter, then the setter");
        order = [];
        assignsIfMissing(counted);
        check(order.join(), "get,get", "an assignment that does not happen calls no setter");
        order = [];
        multiplies(new Proxy({ count: 3 }, {
            get(target, key) { order.push("get " + typeof key + " " + key); return target[key]; },
            set(target, key, value) { order.push("set " + typeof key + " " + key + " " + value); target[key] = value; return true; },
        }));
        check(order.join(), "get string count,set string count 6,get string count", "what the traps of a proxy see");
    }
    check(updatesZero([1]), 2, "a compound assignment to the key \"0\" of an array");
    check(updatesZero(new Uint8Array([255])), 0, "a compound assignment to the key \"0\" of a typed array");
    check(updatesFraction({ 1.5: 1 }), 2, "a compound assignment to the key \"1.5\" of an object");
    check(updatesFraction(new Uint8Array(2)), undefined, "a compound assignment to the key \"1.5\" of a typed array");
    check(updatesAnyKey({ count: 1 }, "count"), 2, "a compound assignment with a key that is not constant");
    {
        let conversions = 0;
        check(updatesAnyKey({ count: 1 }, { toString() { conversions++; return "count"; } }), 2, "a compound assignment with a key that is an object");
        check(conversions, 2, "which is converted once for the assignment and once for the read");
    }

    let indexed = ["zero"];
    indexed[12] = "twelve";
    check(readsZero(indexed), "zero", "the key \"0\" of an array");
    check(readsZero("s"), "s", "the key \"0\" of a string");
    check(readsZero({ 0: "object" }), "object", "the key \"0\" of an object");
    check(readsZero(new Uint8Array([7])), 7, "the key \"0\" of a typed array");
    check(readsTwelve(indexed), "twelve", "the key \"12\" of an array");
    check(readsTwelve([]), undefined, "the key \"12\" of an empty array");
    check(readsLargestIndex({ 4294967294: "largest" }), "largest", "the largest index");
    check(readsBeyondLargestIndex({ 4294967295: "beyond" }), "beyond", "the number after the largest index");
    check(readsLeadingZero({ "01": "a name", 1: "an index" }), "a name", "a key with a leading zero");

    let bytes = new Uint8Array([1, 2, 3]);
    Object.setPrototypeOf(bytes, { __proto__: Uint8Array.prototype, "-0": "inherited", "-1": "inherited", "1.5": "inherited", NaN: "inherited", Infinity: "inherited", "01": "inherited" });
    check(readsMinusZero(bytes), undefined, "\"-0\" of a typed array");
    check(readsMinusOne(bytes), undefined, "\"-1\" of a typed array");
    check(readsFraction(bytes), undefined, "\"1.5\" of a typed array");
    check(readsNaN(bytes), undefined, "\"NaN\" of a typed array");
    check(readsInfinity(bytes), undefined, "\"Infinity\" of a typed array");
    check(readsLeadingZero(bytes), "inherited", "\"01\" of a typed array is an ordinary key");
    check(readsMinusZero({ "-0": "own" }), "own", "\"-0\" of an object");
    check(readsFraction({ 1.5: "own" }), "own", "\"1.5\" of an object");
    check(readsNaN({ NaN: "own" }), "own", "\"NaN\" of an object");
    check(readsInfinity({ Infinity: "own" }), "own", "\"Infinity\" of an object");
    writesFraction(bytes, 9);
    check(Object.hasOwn(bytes, "1.5"), false, "a write of \"1.5\" to a typed array is dropped");
    writesZero(bytes, 9);
    check(bytes[0], 9, "a write of \"0\" to a typed array");
    let target = [];
    writesZero(target, "first");
    check(target.length, 1, "a write of \"0\" to an array lengthens it");

    check(readsUnusualKey({ [["no", "identifier", "of", "this", "program"].join(" ")]: 1 }), 1, "a key that is no identifier");
    check(typeof readsSymbol([]), "function", "a symbol");
    check(readsAnyKey({ name: "any" }, "name"), "any", "a key that is not constant");
    let any = {};
    writesAnyKey(any, "name", "written");
    check(any.name, "written", "a write with a key that is not constant");
    check(readsNumber([0, "one"]), "one", "a constant number");
}

if (aotRemarks("readsName")) {
    const reads = "get-by-val-with-constant-key", writes = "put-by-val-with-constant-key";
    const usesDataStubs = aotRemarks("readsProperty").includes("calls:GetById");
    for (let name of usesDataStubs ? ["readsName", "readsLength", "readsProto", "readsToFixed", "updatesName", "readsAndWritesName", "readsLeadingZero", "increments", "preincrements", "multiplies", "assignsIfMissing", "assignsIfFalsy", "assignsIfTruthy"] : []) {
        if (!aotRemarks(name).includes(reads))
            throw new Error(reads + " does not apply to " + name + ": " + aotRemarks(name).join(" "));
    }
    for (let name of usesDataStubs ? ["writesName", "writesNameStrictly", "writesLength", "writesProto", "readsAndWritesName", "updatesName", "increments", "preincrements", "multiplies", "assignsIfMissing", "assignsIfFalsy", "assignsIfTruthy"] : []) {
        if (!aotRemarks(name).includes(writes))
            throw new Error(writes + " does not apply to " + name + ": " + aotRemarks(name).join(" "));
    }
    for (let name of ["readsZero", "readsTwelve", "readsLargestIndex", "readsBeyondLargestIndex", "readsMinusZero", "readsMinusOne", "readsFraction", "readsNaN", "readsInfinity", "writesZero", "writesFraction", "readsUnusualKey", "readsSymbol", "readsAnyKey", "writesAnyKey", "readsNumber", "updatesZero", "updatesFraction", "updatesAnyKey"]) {
        if (aotRemarks(name).includes(reads) || aotRemarks(name).includes(writes))
            throw new Error("a constant key is assumed in " + name);
    }
}
