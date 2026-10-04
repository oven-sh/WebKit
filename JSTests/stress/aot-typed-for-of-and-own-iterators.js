//@ runDefault("--compileMainScriptAheadOfTime=1", "--useSoundTypes=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useSoundTypes=1", "--useAOTDataStubs=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useSoundTypes=1", "--validateAOTInferredTypes=1")
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

(function () {
    function joined(x) {
        let s = "";
        for (const value of $$t(x, 256))
            s += value + ";";
        return s;
    }
    function hasThree(x) {
        for (const value of $$t(x, 256)) {
            if (value === 3)
                return true;
        }
        return false;
    }
    function rowsWithThree(x) {
        let count = 0;
        x.forEach(function (row) {
            for (const value of $$t(row, 256)) {
                if (value === 3)
                    count++;
            }
        });
        return count;
    }
    function anyRowStartsWithThree(x) {
        return x.some(function (row) {
            for (const value of $$t(row, 256))
                return value === 3;
            return false;
        });
    }
    function attempt(f, x) {
        try {
            return String(f(x));
        } catch (error) {
            return error.constructor.name + ": " + error.message;
        }
    }
    function all(x) { return attempt(joined, x) + " | " + attempt(hasThree, x) + " | " + attempt(rowsWithThree, [[3], x, [3]]) + " | " + attempt(anyRowStartsWithThree, [[1], x, [3]]); }
    const refused = "TypeError: Type check failed: expected an array that is iterated over like any other";
    const allRefused = refused + " | " + refused + " | " + refused + " | " + refused;

    check(all([1, 2, 3]), "1;2;3; | true | 3 | true", "an original array");
    check(all([1, , 3]), "1;undefined;3; | true | 3 | true", "an original array with a hole");
    check(all([]), " | false | 2 | true", "an empty array");
    class Plain extends Array { }
    check(all(Plain.of(1, 2, 3)), "1;2;3; | true | 3 | true", "an instance of a subclass");
    const withProperty = [1, 2, 3];
    withProperty.extra = true;
    check(all(withProperty), "1;2;3; | true | 3 | true", "an array with a property of its own");

    const own = [1, 2, 3];
    own[Symbol.iterator] = function* () { yield 9; };
    check(all(own), allRefused, "an array with an iterator method of its own");

    const reparented = [1, 2, 3];
    Object.setPrototypeOf(reparented, { __proto__: Array.prototype, *[Symbol.iterator]() { yield 5; } });
    check(all(reparented), allRefused, "an array that inherits another iterator method");

    class Reversed extends Array {
        *[Symbol.iterator]() {
            for (let i = this.length; i--;)
                yield this[i];
        }
    }
    check(all(Reversed.of(1, 2, 3)), allRefused, "an instance of a subclass with an iterator method");

    const log = [];
    const watched = [1, 2, 3];
    Object.setPrototypeOf(watched, new Proxy(Array.prototype, {
        get(target, key, receiver) {
            log.push(String(key));
            return Reflect.get(target, key, receiver);
        },
    }));
    check(attempt(hasThree, watched), "true", "an array with a proxy for a prototype");
    check(log.join(), "Symbol(Symbol.iterator)", "what the proxy was asked for");

    const foreign = createGlobalObject().Array(1, 2, 3);
    check(all(foreign), allRefused, "an array of another realm");

    for (const f of [joined, hasThree, rowsWithThree, anyRowStartsWithThree])
        applies(f, "calls:operationAOTArrayIteratorMethod");
    applies(rowsWithThree, "inlined-builtin");
    applies(anyRowStartsWithThree, "inlined-builtin");
    doesNotApply(anyRowStartsWithThree, "array-view");
})();
