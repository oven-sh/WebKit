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
function repeat(f) {
    for (let i = 0; i < 100; i++)
        f(i);
}

function program() {
function forEach(array, callback) {
    if (array !== undefined) {
        for (let i = 0; i < array.length; i++) {
            const result = callback(array[i], i);
            if (result)
                return result;
        }
    }
    return undefined;
}
function some(array, predicate) {
    if (array !== undefined) {
        if (predicate !== undefined) {
            for (let i = 0; i < array.length; i++) {
                if (predicate(array[i]))
                    return true;
            }
        } else
            return array.length > 0;
    }
    return false;
}
function mapOf(array, f) {
    let result;
    if (array !== undefined) {
        result = [];
        for (let i = 0; i < array.length; i++)
            result.push(f(array[i], i));
    }
    return result;
}
function ignoresCallback(array, callback) {
    let sum = 0;
    for (let i = 0; i < array.length; i++)
        sum += array[i] * 2 + i - (array[i] >> 1) + (array[i] & 3) + (i % 3) * (array[i] | 1);
    return sum;
}
function replacesCallback(array, callback) {
    callback = x => x + 100;
    let sum = 0;
    for (let i = 0; i < array.length; i++)
        sum += callback(array[i]);
    return sum;
}
function passesCallbackOn(array, callback) {
    return array.map(callback).join() + array.length + (array.length > 2 ? "long" : "short") + (array[0] | 0) + (array[1] | 0) + (array[2] | 0);
}
function isHuge(array, callback) {
    let s = 0;
    for (let i = 0; i < array.length; i++) {
        const v = callback(array[i]);
        s += v; s ^= v << 1; s += v >> 1; s -= v & 7; s += v % 5; s ^= v | 9; s += v * 3; s -= v >>> 2; s += v + 11; s ^= v - 13;
        s += v; s ^= v << 2; s += v >> 2; s -= v & 15; s += v % 7; s ^= v | 17; s += v * 5; s -= v >>> 3; s += v + 19; s ^= v - 23;
        s += v; s ^= v << 3; s += v >> 3; s -= v & 31; s += v % 11; s ^= v | 33; s += v * 7; s -= v >>> 4; s += v + 29; s ^= v - 31;
        s += v; s ^= v << 4; s += v >> 4; s -= v & 63; s += v % 13; s ^= v | 65; s += v * 9; s -= v >>> 5; s += v + 37; s ^= v - 41;
    }
    return s;
}

function findsLarge(array, limit) { return forEach(array, x => x > limit ? x : undefined); }
function findsIndex(array, wanted) { return forEach(array, (x, i) => x === wanted ? i + 1 : 0); }
function hasEven(array) { return some(array, x => (x & 1) === 0); }
function doubles(array) { return mapOf(array, x => x * 2); }
function sumsThroughCallback(array) { let sum = 0; forEach(array, x => { sum += x; }); return sum; }
const named = x => x > 2 ? x : undefined;
function passesVariable(array) { return forEach(array, named); }
function passesParameter(array, callback) { return forEach(array, callback); }
function passesToIgnoring(array) { return ignoresCallback(array, x => x); }
function passesToReplacing(array) { return replacesCallback(array, x => x); }
function passesToPassingOn(array) { return passesCallbackOn(array, x => x + 1); }
function passesToHuge(array) { return isHuge(array, x => x + 1); }
function passesNothing(array) { return some(array); }
function throwsFromCallback(array) { return forEach(array, x => { if (x === 3) throw new RangeError("three"); }); }
function nests(arrays) { return forEach(arrays, inner => forEach(inner, x => x > 4 ? x : undefined)); }

applies(findsLarge, "inlined-call-with-callback:forEach", "inlined-closure");
applies(findsIndex, "inlined-call-with-callback:forEach", "inlined-closure");
applies(hasEven, "inlined-call-with-callback:some", "inlined-closure");
applies(doubles, "inlined-call-with-callback:mapOf", "inlined-closure");
applies(sumsThroughCallback, "inlined-call-with-callback:forEach");
applies(throwsFromCallback, "inlined-call-with-callback:forEach");
applies(nests, "inlined-call-with-callback:forEach");
for (const f of [passesVariable, passesParameter, passesToIgnoring, passesToReplacing, passesToPassingOn, passesToHuge, passesNothing])
    doesNotApply(f, "inlined-call-with-callback");

repeat(i => {
    check(findsLarge([1, 5, 9], 4), 5, "the first that is large");
    check(findsLarge([1, 2, 3], 4), undefined, "none is large");
    check(findsLarge([], 4), undefined, "an empty array");
    check(findsLarge(undefined, 4), undefined, "no array");
    check(findsLarge([i, i + 1], i), i + 1, "a limit that changes");
    check(findsIndex([7, 8, 9], 9), 3, "an index");
    check(findsIndex([7, 8, 9], 1), undefined, "no index");
    check(hasEven([1, 3, 4]), true, "some are even");
    check(hasEven([1, 3, 5]), false, "none is even");
    check(doubles([1, 2, i]).join(), "2,4," + 2 * i, "a map");
    check(doubles(undefined), undefined, "a map of nothing");
    check(sumsThroughCallback([1, 2, i]), 3 + i, "a callback that writes a variable of its caller");
    check(passesVariable([1, 2, 3]), 3, "a callback in a variable");
    check(passesParameter([1, 2, 3], x => x === 2 ? "two" : undefined), "two", "a callback that is a parameter");
    check(passesToIgnoring([1, 2]), ignoresCallback([1, 2]), "a callee that ignores the callback");
    check(passesToReplacing([1, 2]), 203, "a callee that replaces the callback");
    check(passesToPassingOn([1, 2]), "2,32short120", "a callee that passes the callback on");
    check(passesToHuge([1, 2]), isHuge([1, 2], x => x + 1), "a callee that is too big");
    check(passesNothing([1]), true, "no callback");
    check(passesNothing([]), false, "no callback and nothing");
    check(nests([[1, 2], [3, 9]]), 9, "nested");
    check(nests([[1, 2], [3, 4]]), undefined, "nested, none");
    let caught;
    try {
        throwsFromCallback([1, 2, 3, 4]);
    } catch (error) {
        caught = error;
    }
    check(caught instanceof RangeError && caught.message, "three", "an exception from the callback");
});

{
    const holey = [1, , 3];
    check(sumsThroughCallback([1.5, 2.5]), 4, "doubles");
    check(String(doubles(holey)), "2,NaN,6", "a hole");
    check(findsLarge({ length: 2, 0: 1, 1: 9 }, 4), 9, "an object that is like an array");
    check(findsLarge("19", "4"), "9", "a string");
    const growing = [1, 2];
    let seen = 0;
    forEach(growing, x => { if (seen++ < 3) growing.push(x); });
    check(growing.length, 5, "an array that grows while it is walked");
}

{
    let error;
    try {
        throwsFromCallback([3]);
    } catch (e) {
        error = e;
    }
    const stack = String(error.stack);
    check(stack.includes("forEach"), true, "the inlined callee is in the stack trace: " + stack);
    check(stack.includes("throwsFromCallback"), true, "its caller is in the stack trace: " + stack);
}
}
program();
