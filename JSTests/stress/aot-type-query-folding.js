//@ runDefault("--compileMainScriptAheadOfTime=1")
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
const folds = "folded-type-query", withoutMasqueradeTest = "null-comparison-without-masquerade-test", byIdentity = "truthiness-of-string-by-identity";

function objectOrNullIsObject(i) { let v = i === 0 ? { a: 1 } : null; return typeof v === "object"; }
function collectionsAreObjects(i) { let v = i === 0 ? new Map() : i === 1 ? new Set() : i === 2 ? /a/ : i === 3 ? new Date(0) : i === 4 ? [] : new WeakMap(); return typeof v === "object"; }
function objectIsNoFunction(i) { let v = i === 0 ? { a: 1 } : i === 1 ? [] : i === 2 ? new Map() : undefined; return typeof v === "function"; }
function functionIsNoObject(i) { let v = i === 0 ? () => 1 : i === 1 ? "text" : 5; return typeof v === "object"; }
function stringIsString(text) { let v = "" + text; return typeof v === "string"; }
function numberIsNoString(i) { let v = i === 0 ? 5 : i === 1 ? 2.5 : undefined; return typeof v === "string"; }
function objectIsNoString(i) { let v = i === 0 ? { a: 1 } : null; return typeof v === "string"; }
function symbolIsSymbol() { let v = Symbol.iterator; return typeof v === "symbol"; }
function stringIsNoSymbol(text) { let v = "" + text; return typeof v === "symbol"; }
const folded = [objectOrNullIsObject, collectionsAreObjects, objectIsNoFunction, functionIsNoObject, stringIsString, numberIsNoString, objectIsNoString, symbolIsSymbol, stringIsNoSymbol];
for (let f of folded)
    noInline(f);

check([0, 1].map(objectOrNullIsObject).join(), "true,true", "an object or null");
check([0, 1, 2, 3, 4, 5].map(collectionsAreObjects).join(), "true,true,true,true,true,true", "objects of built-in classes");
check([0, 1, 2, 3].map(objectIsNoFunction).join(), "false,false,false,false", "what is never callable");
check([0, 1, 2].map(functionIsNoObject).join(), "false,false,false", "a function, a string or a number");
check([stringIsString("a"), stringIsString(""), stringIsString(5)].join(), "true,true,true", "a string");
check([0, 1, 2].map(numberIsNoString).join(), "false,false,false", "a number or undefined");
check([0, 1].map(objectIsNoString).join(), "false,false", "an object or null is no string");
check(symbolIsSymbol(), true, "a symbol");
check(stringIsNoSymbol("a"), false, "a string is no symbol");
for (let f of folded)
    applies(f, folds);

function kindOfAnything(v) { return [typeof v === "object", typeof v === "function", typeof v === "string", typeof v === "symbol", typeof v === "undefined", v == null].map(Number).join(""); }
function objectOrUndefinedIsObject(i) { let v = i === 0 ? { a: 1 } : undefined; return typeof v === "object"; }
function functionOrObjectIsFunction(i) { let v = i === 0 ? () => 1 : { a: 1 }; return typeof v === "function"; }
function stringOrNumberIsString(i, text) { let v = i === 0 ? "" + text : 5; return typeof v === "string"; }
function stringOrObjectIsString(i, text) { let v = i === 0 ? "" + text : { a: 1 }; return typeof v === "string"; }
const tested = [kindOfAnything, objectOrUndefinedIsObject, functionOrObjectIsFunction, stringOrNumberIsString, stringOrObjectIsString];
for (let f of tested)
    noInline(f);

const anything = [undefined, null, true, 0, "", "a", Symbol.iterator, 1n, { }, [], () => 1, function () { }, class { }, /a/, new Map(), new Proxy({ }, { }), new Proxy(function () { }, { }), (function () { }).bind(null), new String("a"), Math.max, Object];
check(anything.map(kindOfAnything).join(), "000011,100001,000000,000000,001000,001000,000100,000000,100000,100000,010000,010000,010000,100000,100000,100000,010000,010000,100000,010000,010000", "typeof anything");
if (typeof makeMasquerader === "function")
    check(kindOfAnything(makeMasquerader()), "000011", "typeof an object that masquerades as undefined");
check([0, 1].map(objectOrUndefinedIsObject).join(), "true,false", "an object or undefined");
check([0, 1].map(functionOrObjectIsFunction).join(), "true,false", "a function or an object");
check([0, 1].map(i => stringOrNumberIsString(i, "a")).join(), "true,false", "a string or a number");
check([0, 1].map(i => stringOrObjectIsString(i, "a")).join(), "true,false", "a string or an object");
for (let f of tested)
    doesNotApply(f, folds);

function objectEqualsNull(i) { let v = i === 0 ? { a: 1 } : i === 1 ? undefined : null; return v == null; }
function arrayDiffersFromNull(i) { let v = i === 0 ? [] : i === 1 ? undefined : null; return v != null ? "given" : "missing"; }
function mapIsUndefined(i) { let v = i === 0 ? new Map() : i === 1 ? undefined : null; return typeof v === "undefined"; }
function objectNeverEqualsNull(i) { let v = i === 0 ? { a: 1 } : [1]; return v == null; }
function functionEqualsNull(i) { let v = i === 0 ? () => 1 : undefined; return v == null; }
function anythingEqualsNull(v) { return v == null; }
function numberEqualsNull(i) { let v = i === 0 ? 5 : null; return v == null; }
for (let f of [objectEqualsNull, arrayDiffersFromNull, mapIsUndefined, objectNeverEqualsNull, functionEqualsNull, anythingEqualsNull, numberEqualsNull])
    noInline(f);

check([0, 1, 2].map(objectEqualsNull).join(), "false,true,true", "an object, undefined or null compared with null");
check([0, 1, 2].map(arrayDiffersFromNull).join(), "given,missing,missing", "an array, undefined or null compared with null");
check([0, 1, 2].map(mapIsUndefined).join(), "false,true,false", "typeof a map, undefined or null");
check([0, 1].map(objectNeverEqualsNull).join(), "false,false", "objects compared with null");
check([0, 1].map(functionEqualsNull).join(), "false,true", "a function or undefined compared with null");
check(anything.map(anythingEqualsNull).map(Number).join(""), "110000000000000000000", "anything compared with null");
check([0, 1].map(numberEqualsNull).join(), "false,true", "a number or null compared with null");
for (let f of [objectEqualsNull, arrayDiffersFromNull, mapIsUndefined, objectNeverEqualsNull])
    applies(f, withoutMasqueradeTest);
for (let f of [functionEqualsNull, anythingEqualsNull, numberEqualsNull])
    doesNotApply(f, withoutMasqueradeTest);

function isStringTruthy(text) { let s = "" + text; return s ? "truthy" : "falsy"; }
function isOptionalStringTruthy(i, text) { let s = i === 0 ? "" + text : i === 1 ? undefined : null; return s ? "truthy" : "falsy"; }
function isStringOrFlagTruthy(i, text) { let s = i === 0 ? "" + text : i === 1; return s ? "truthy" : "falsy"; }
function isStringOrObjectTruthy(i, text) { let s = i === 0 ? "" + text : i === 1 ? { } : undefined; return !s ? "falsy" : "truthy"; }
function isSliceTruthy(text, from, to) { let s = ("" + text).slice(from | 0, to | 0); return s ? "truthy" : "falsy"; }
function isSumTruthy(a, b) { let s = ("" + a) + ("" + b); return s ? "truthy" : "falsy"; }
function isObjectTruthy(i) { let o = i === 0 ? { } : undefined; return o ? "truthy" : "falsy"; }
function isNumberTruthy(n) { let x = +n; return x ? "truthy" : "falsy"; }
const strings = [isStringTruthy, isOptionalStringTruthy, isStringOrFlagTruthy, isStringOrObjectTruthy, isSliceTruthy, isSumTruthy];
for (let f of [...strings, isObjectTruthy, isNumberTruthy])
    noInline(f);

const emptyStrings = ["", String(""), [].join(), "x".repeat(0), JSON.parse('""'), `${""}`, "abc".substring(3), "abc".slice(1, 1), "Ā".slice(1), "a".replace("a", ""), " ".trim(), "" + "", String.fromCharCode(), new String("").valueOf(), [""][0], "a,".split(",")[1]];
const otherStrings = ["a", " ", "0", "false", "\0", "Ā", "ab".slice(1), "a" + "b", "undefined", "x".repeat(100)];
for (let text of emptyStrings) {
    check(isStringTruthy(text), "falsy", "an empty string");
    check(isOptionalStringTruthy(0, text), "falsy", "an empty string that may be missing");
    check(isStringOrFlagTruthy(0, text), "falsy", "an empty string that may be a boolean");
    check(isStringOrObjectTruthy(0, text), "falsy", "an empty string that may be an object");
    check(isSumTruthy(text, text), "falsy", "a sum of empty strings");
}
for (let text of otherStrings) {
    check(isStringTruthy(text), "truthy", "a string that is not empty");
    check(isOptionalStringTruthy(0, text), "truthy", "a string that may be missing");
    check(isStringOrFlagTruthy(0, text), "truthy", "a string that may be a boolean");
    check(isStringOrObjectTruthy(0, text), "truthy", "a string that may be an object");
    check(isSumTruthy(text, "") + isSumTruthy("", text), "truthytruthy", "a sum with an empty string");
}
check(isOptionalStringTruthy(1, "a") + isOptionalStringTruthy(2, "a"), "falsyfalsy", "undefined and null in place of a string");
check(isStringOrFlagTruthy(1, "") + isStringOrFlagTruthy(2, "a"), "truthyfalsy", "true and false in place of a string");
check(isStringOrObjectTruthy(1, "") + isStringOrObjectTruthy(2, "a"), "truthyfalsy", "an object and undefined in place of a string");
check([[0, 0], [0, 1], [1, 1], [3, 3], [2, 1], [5, 9], [-1, 3]].map(([from, to]) => isSliceTruthy("abc", from, to)).join(), "falsy,truthy,falsy,falsy,falsy,falsy,truthy", "slices");
check(isObjectTruthy(0) + isObjectTruthy(1), "truthyfalsy", "an object or undefined");
check(isNumberTruthy(1) + isNumberTruthy(0), "truthyfalsy", "a number");
for (let f of strings) {
    applies(f, byIdentity);
    doesNotApply(f, "calls:ToBoolean");
}
doesNotApply(isObjectTruthy, byIdentity);
doesNotApply(isNumberTruthy, byIdentity);
