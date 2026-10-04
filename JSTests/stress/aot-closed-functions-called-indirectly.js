//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTDataStubs=0")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
const isCompiled = !!aotRemarks("check");
const doesNotEscape = "function-does-not-escape";

let escaped;
function run(n) {
    function closedSum(a, b) { return a > 100 ? closedSum(a - 1, b) : a + b; }
    function closedAndInlined(a) { return a + 1; }
    let count = 0;
    function closedWithoutParameters() { return ++count > 1000 ? closedWithoutParameters() : 1; }
    function openSum(a, b) { return a + b; }
    escaped = openSum;
    return closedSum(n, 2) + closedSum(n + 0.5, 4) + closedAndInlined(n) + closedWithoutParameters() + openSum(n, 2);
}
for (let i = 0; i < 3; ++i)
    check(run(1), 3 + 5.5 + 2 + 1 + 3, "direct calls");
check(escaped(1, 2), 3, "the function that escapes");

if (isCompiled) {
    check(aotRemarks("closedSum").includes(doesNotEscape), true, "closedSum does not escape");
    check(aotRemarks("closedAndInlined").includes(doesNotEscape), true, "closedAndInlined does not escape");
    check(aotRemarks("closedWithoutParameters").includes(doesNotEscape), true, "closedWithoutParameters does not escape");
    check(aotRemarks("openSum").includes(doesNotEscape), false, "openSum does not escape");
}

const routes = {
    "a call": f => f(1, 2),
    "a call without arguments": f => f(),
    "a call with values of other types": f => f("1", { }),
    "a call with many arguments": f => f(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14),
    "a call with spread": f => f(...[1, 2]),
    "a tail call": f => (function () { "use strict"; return f(1, 2); })(),
    "a method call": f => ({ f }).f(1, 2),
    "Function.prototype.call": f => f.call(null, 1, 2),
    "Function.prototype.apply": f => f.apply(null, [1, 2]),
    "Reflect.apply": f => Reflect.apply(f, null, [1, 2]),
    "a bound function": f => f.bind(null, 1)(2),
    "a bound function with a bound this only": f => f.bind({ })(1, 2),
    "Array.prototype.map": f => [1].map(f)[0],
    "Array.prototype.reduce": f => [1, 2].reduce(f),
    "a proxy": f => new Proxy(f, { })(1, 2),
    "a getter": f => Object.defineProperty({ }, "x", { get: f }).x,
    "an inherited getter": f => Object.create(Object.defineProperty({ }, "x", { get: f })).x,
    "a setter": f => { Object.defineProperty({ }, "x", { set: f }).x = 1; },
    "a conversion": f => `${{ toString: f }}`,
    "interpreted code": f => new Function("f", "return f(1, 2)")(f),
    "C++": f => $vm.callFromCPP(f, 1, 1, 2),
    "a cached call from C++": f => $vm.cachedCallFromCPP(f, 1, 1, 2),
    "a reaction of a promise": f => {
        let outcome;
        Promise.resolve(1).then(f).then(value => { outcome = { value }; }, error => { outcome = { error }; });
        drainMicrotasks();
        if ("error" in outcome)
            throw outcome.error;
        return outcome.value;
    },
};

function outcomeOf(route, f) {
    try {
        return { value: route(f) };
    } catch (error) {
        return { error };
    }
}

for (let name of ["closedSum", "closedAndInlined", "closedWithoutParameters"]) {
    let functions = aotFunctionsNamed(name);
    check(functions.length > 0, isCompiled, "there are function objects for " + name);
    for (let f of functions) {
        check(typeof f, "function", "typeof");
        check(f.name, name, "name");
        for (let [what, route] of Object.entries(routes)) {
            for (let i = 0; i < 12; ++i) {
                let { error } = outcomeOf(route, f);
                check(error instanceof TypeError, true, name + " through " + what + " throws a TypeError");
                check(error.message, "The function '" + name + "' was compiled for direct calls only", "the message through " + what);
            }
        }
        for (let construct of [() => new f(1, 2), () => Reflect.construct(f, [1, 2])]) {
            let { value, error } = outcomeOf(construct);
            check(error ? error instanceof TypeError : typeof value === "object", true, "constructing " + name);
        }
    }
}

for (let f of aotFunctionsNamed("openSum")) {
    check(f(1, 2), 3, "openSum through a call");
    check(f("1", 2), "12", "openSum with values of other types");
    check(f.call(null, 1, 2), 3, "openSum through call");
    check(f.bind(null, 1)(2), 3, "openSum through a bound function");
    check([1, 2].reduce(f), 3, "openSum through reduce");
    check(new Function("f", "return f(1, 2)")(f), 3, "openSum through interpreted code");
}

for (let i = 0; i < 3; ++i)
    check(run(1), 3 + 5.5 + 2 + 1 + 3, "direct calls afterwards");
