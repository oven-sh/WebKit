//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTDataStubs=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--validateAOTInferredTypes=1")
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
function readsProperty(o) { return o.property; }
readsProperty({ property: 1 });
const usesDataStubs = (remarksOf(readsProperty) || []).includes("calls:GetById");

(function () {
    function constructed(size) { return new Array(size); }
    function called(size) { return Array(size); }
    function described(a) {
        let s = a.length + ":";
        for (let i = 0; i < a.length && i < 4; i++)
            s += (i in a ? String(a[i]) : "hole") + ",";
        return s + Object.keys(a).length;
    }
    function attempt(make, size) {
        try {
            return described(make(size));
        } catch (error) {
            return error.constructor.name;
        }
    }
    function both(size) {
        const first = attempt(constructed, size);
        check(attempt(called, size), first, "Array(" + String(size) + ") against new Array of the same");
        return first;
    }
    function lengthsInLoop(count, size) {
        let total = 0;
        for (let i = 0; i < count; i++)
            total += new Array(size).length;
        return total;
    }
    function literalsInLoop(count) {
        let total = 0;
        for (let i = 0; i < count; i++)
            total += [i, i].length;
        return total;
    }
    function filled(size) {
        const a = new Array(size);
        for (let i = 0; i < size; i++)
            a[i] = i * 2;
        return a.join();
    }
    function filledWithAnything(size) {
        const a = new Array(size);
        for (let i = 0; i < size; i++)
            a[i] = i & 1 ? "s" + i : i + 0.5;
        return a.join();
    }
    function pushedOnto(size) {
        const a = new Array(size);
        a.push("last");
        return described(a);
    }

    check(both(0), "0:0", "no elements");
    check(both(1), "1:hole,0", "one hole");
    check(both(3), "3:hole,hole,hole,0", "three holes");
    check(both(6), "6:hole,hole,hole,hole,0", "six holes");
    check(both(1000), "1000:hole,hole,hole,hole,0", "a thousand holes");
    check(both(1024), "1024:hole,hole,hole,hole,0", "more than fits a small allocation");
    check(both(100000), "100000:hole,hole,hole,hole,0", "a hundred thousand holes");
    check(both(4294967295), "4294967295:hole,hole,hole,hole,0", "the longest array");
    check(both(-1), "RangeError", "a negative size");
    check(both(1.5), "RangeError", "a fraction");
    check(both(4294967296), "RangeError", "too long");
    check(both(NaN), "RangeError", "NaN");
    check(both(-0), "0:0", "negative zero");
    check(both(3.0), "3:hole,hole,hole,0", "an integral double");
    check(both("3"), "1:3,1", "a string");
    check(both(undefined), "1:undefined,1", "undefined");
    check(both(null), "1:null,1", "null");
    check(both(true), "1:true,1", "a boolean");
    check(attempt(constructed, { valueOf() { return 2; } }), "1:[object Object],1", "an object");

    check(lengthsInLoop(1000, 3), 3000, "many arrays of three");
    check(lengthsInLoop(1000, 0), 0, "many empty arrays");
    check(lengthsInLoop(100, 700), 70000, "many long arrays");
    check(literalsInLoop(10), 20, "literals");
    check(filled(5), "0,2,4,6,8", "filled with integers");
    check(filled(0), "", "nothing to fill");
    check(filledWithAnything(4), "0.5,s1,2.5,s3", "filled with doubles and strings");
    check(pushedOnto(2), "3:hole,hole,last,1", "a push after the holes");
    check(pushedOnto(0), "1:last,1", "a push onto an empty array");
    check(Object.getPrototypeOf(constructed(2)), Array.prototype, "the prototype");
    check(Array.isArray(called(2)), true, "an array");

    const helper = "allocates-sized-array-through-helper";
    for (const f of [constructed, called, lengthsInLoop, filled, pushedOnto]) {
        if (usesDataStubs)
            applies(f, helper);
        else
            doesNotApply(f, helper);
    }
    doesNotApply(literalsInLoop, helper);

    Array.prototype[1] = "inherited";
    const inheriting = constructed(3);
    check(String(inheriting[1]) + " " + (1 in inheriting) + " " + Object.hasOwn(inheriting, 1), "inherited true false", "a hole over an element of Array.prototype");
    delete Array.prototype[1];

    Object.defineProperty(Array.prototype, 2, { set(value) { this.seen = value; }, configurable: true });
    const intercepted = constructed(4);
    intercepted[2] = "stored";
    check(intercepted.seen + " " + Object.hasOwn(intercepted, 2), "stored false", "a store over a setter of Array.prototype");
    check(lengthsInLoop(10, 3), 30, "arrays made afterwards");
    delete Array.prototype[2];
})();
