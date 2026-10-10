//@ runDefault("--compileMainScriptAheadOfTime=1", "--useSoundTypes=1")
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
function nameOfErrorThrownBy(f, ...args) {
    try {
        f(...args);
    } catch (error) {
        return error.constructor.name;
    }
    return "nothing";
}
const onUsualPath = "cold-call-on-usual-path";

function checksNumber(a) { return $$t(a, 8); }
function checksString(a) { return $$t(a, 16); }
function checksCallable(f) { return $$t(f, 128); }
function checksTwoNumbers(a, b) { return $$t(a, 8) + $$t(b, 8); }
function checksInLoop(values) {
    let sum = 0;
    for (let i = 0; i < values.length; ++i)
        sum += $$t(values[i], 8);
    return sum;
}
function makeReader(readsFirst) {
    function readsVariableOfOuterFunction() { return value; }
    if (readsFirst)
        readsVariableOfOuterFunction();
    let value = 1;
    return readsVariableOfOuterFunction;
}

for (let i = 0; i < 100; ++i) {
    check(checksNumber(i), i, "an integer");
    check(checksNumber(i + 0.5), i + 0.5, "a double");
    check(checksString("s" + i), "s" + i, "a string");
    check(checksCallable(check), check, "a function");
    check(checksTwoNumbers(i, 0.5), i + 0.5, "two numbers");
    check(checksInLoop([i, 1, 0.5]), i + 1.5, "numbers in an array");
    check(makeReader(false)(), 1, "a variable that is initialized");
}
check(nameOfErrorThrownBy(checksNumber, "1"), "TypeError", "a string is not a number");
check(nameOfErrorThrownBy(checksNumber, undefined), "TypeError", "undefined is not a number");
check(nameOfErrorThrownBy(checksString, 1), "TypeError", "a number is not a string");
check(nameOfErrorThrownBy(checksCallable, { }), "TypeError", "an object is not callable");
check(nameOfErrorThrownBy(checksTwoNumbers, 1, null), "TypeError", "null is not a number");
check(nameOfErrorThrownBy(checksInLoop, [1, "2"]), "TypeError", "a string among numbers");
check(nameOfErrorThrownBy(makeReader, true), "ReferenceError", "a variable that is not initialized yet");

doesNotApply(checksNumber, onUsualPath);
doesNotApply(checksString, onUsualPath);
doesNotApply(checksCallable, onUsualPath);
doesNotApply(checksTwoNumbers, onUsualPath);
doesNotApply(checksInLoop, onUsualPath);
applies(makeReader(false), onUsualPath);
