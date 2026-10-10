//@ runDefault("--compileMainScriptAheadOfTime=1", "--numberOfAOTCompilerThreads=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--numberOfAOTCompilerThreads=1", "--seedOfAOTInferenceOrderForTesting=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--numberOfAOTCompilerThreads=1", "--seedOfAOTInferenceOrderForTesting=3")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--numberOfAOTCompilerThreads=1", "--seedOfAOTInferenceOrderForTesting=12")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--numberOfAOTCompilerThreads=1", "--seedOfAOTInferenceOrderForTesting=13")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--numberOfAOTCompilerThreads=1", "--seedOfAOTInferenceOrderForTesting=26")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--numberOfAOTCompilerThreads=1", "--seedOfAOTInferenceOrderForTesting=32")
//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--numberOfAOTCompilerThreads=1", "--useAOTInlining=0")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function remarksOf(name) {
    return aotRemarks(name).slice().sort().join(" ");
}

(function () {
    function lateFirst() { return middleFirst(0, "abc"); }
    function earlyFirst() { return middleFirst(0, undefined) + middleFirst(0, null) + middleFirst(0, true) + middleFirst(0, 1) + middleFirst(0, 1.5); }
    function calleeFirst(x) { return x ? 1 : 2; }
    function middleFirst(k, p) {
        switch (k) {
        case 0:
            let v = p;
        case 1:
            if (v !== undefined && v !== null && !Number.isInteger(v) && typeof v === "boolean")
                return calleeFirst(v);
        }
        return 0;
    }
    check(earlyFirst() + lateFirst(), 1, "five tests of one value, the first copy");
})();

(function () {
    function lateSecond() { return middleSecond(0, "abc"); }
    function earlySecond() { return middleSecond(0, undefined) + middleSecond(0, null) + middleSecond(0, true) + middleSecond(0, 1) + middleSecond(0, 1.5); }
    function calleeSecond(x) { return x ? 1 : 2; }
    function middleSecond(k, p) {
        switch (k) {
        case 0:
            let v = p;
        case 1:
            if (v !== undefined && v !== null && !Number.isInteger(v) && typeof v === "boolean")
                return calleeSecond(v);
        }
        return 0;
    }
    check(earlySecond() + lateSecond(), 1, "five tests of one value, the second copy");
})();

(function () {
    function lateThird() { return middleThird(0, "abc"); }
    function earlyThird() { return middleThird(0, undefined) + middleThird(0, null) + middleThird(0, true) + middleThird(0, 1) + middleThird(0, 1.5); }
    function calleeThird(x) { return x ? 1 : 2; }
    function middleThird(k, p) {
        switch (k) {
        case 0:
            let v = p;
        case 1:
            if (v !== undefined && v !== null && !Number.isInteger(v) && typeof v === "boolean")
                return calleeThird(v);
        }
        return 0;
    }
    check(earlyThird() + lateThird(), 1, "five tests of one value, the third copy");
})();

(function () {
    function lateFourth() { return middleFourth(0, "abc"); }
    function earlyFourth() { return middleFourth(0, undefined) + middleFourth(0, null) + middleFourth(0, true) + middleFourth(0, 1) + middleFourth(0, 1.5); }
    function calleeFourth(x) { return x ? 1 : 2; }
    function middleFourth(k, p) {
        switch (k) {
        case 0:
            let v = p;
        case 1:
            if (v !== undefined && v !== null && !Number.isInteger(v) && typeof v === "boolean")
                return calleeFourth(v);
        }
        return 0;
    }
    check(earlyFourth() + lateFourth(), 1, "five tests of one value, the fourth copy");
})();

(function () {
    function callsLinked() { return middleLinked(1, true) + middleLinked("abc", true); }
    function calleeLinked(x) { return x ? 1 : 2; }
    function middleLinked(p, isFirst) {
        if (typeof p === "string")
            return 0;
        let second = isFirst ? p : [1];
        if (Array.isArray(second))
            return 0;
        let third = isFirst ? second : "abc";
        if (typeof third === "string")
            return 0;
        let fourth = isFirst ? third : [1];
        if (Array.isArray(fourth))
            return 0;
        let fifth = isFirst ? fourth : "abc";
        if (typeof fifth === "string")
            return 0;
        let sixth = isFirst ? fifth : [1];
        if (Array.isArray(sixth))
            return 0;
        return calleeLinked(sixth);
    }
    check(callsLinked(), 1, "six tests, each of a value that depends on the test before");
})();

(function () {
    function callsUnnarrowed() { return middleUnnarrowed(undefined) + middleUnnarrowed("abc") + middleUnnarrowed({ length: 1, charCodeAt() { return 1; } }); }
    function calleeUnnarrowed(x) { return x.length + x.charCodeAt(0); }
    function middleUnnarrowed(e) { if (e !== undefined && typeof e !== "string") return calleeUnnarrowed(e); return 0; }
    check(callsUnnarrowed(), 2, "a test that is not worth narrowing for, behind one that is");
})();

(function () {
    function callsWithBooleans() { return passesBooleans(true); }
    function givenBooleans(x) { return x ? 1 : 2; }
    function passesBooleans(e) { return givenBooleans(e); }
    check(callsWithBooleans(), 1, "givenBooleans");
})();

(function () {
    function callsWithBooleansOrDoubles() { return passesBooleansOrDoubles(true) + passesBooleansOrDoubles(1.5); }
    function givenBooleansOrDoubles(x) { return x ? 1 : 2; }
    function passesBooleansOrDoubles(e) { return givenBooleansOrDoubles(e); }
    check(callsWithBooleansOrDoubles(), 2, "givenBooleansOrDoubles");
})();

(function () {
    function callsWithIntegers() { return passesIntegers(1); }
    function givenIntegers(x) { return x ? 1 : 2; }
    function passesIntegers(e) { return givenIntegers(e); }
    check(callsWithIntegers(), 1, "givenIntegers");
})();

(function () {
    function callsWithIntegersStringsOrArrays() { return passesIntegersStringsOrArrays(1) + passesIntegersStringsOrArrays("abc") + passesIntegersStringsOrArrays([1]); }
    function givenIntegersStringsOrArrays(x) { return x ? 1 : 2; }
    function passesIntegersStringsOrArrays(e) { return givenIntegersStringsOrArrays(e); }
    check(callsWithIntegersStringsOrArrays(), 3, "givenIntegersStringsOrArrays");
})();

(function () {
    function callsWithObjects() { return passesObjects({ length: 1, charCodeAt() { return 1; } }); }
    function givenObjects(x) { return x.length + x.charCodeAt(0); }
    function passesObjects(e) { return givenObjects(e); }
    check(callsWithObjects(), 2, "givenObjects");
})();

(function () {
    function callsWithStringsOrObjects() { return passesStringsOrObjects("abc") + passesStringsOrObjects({ length: 1, charCodeAt() { return 1; } }); }
    function givenStringsOrObjects(x) { return x.length + x.charCodeAt(0); }
    function passesStringsOrObjects(e) { return givenStringsOrObjects(e); }
    check(callsWithStringsOrObjects(), 102, "givenStringsOrObjects");
})();

check(remarksOf("givenBooleans") !== remarksOf("givenBooleansOrDoubles"), true, "code for booleans differs from code for booleans or doubles");
check(remarksOf("givenIntegers") !== remarksOf("givenIntegersStringsOrArrays"), true, "code for integers differs from code for integers, strings or arrays");
check(remarksOf("givenObjects") !== remarksOf("givenStringsOrObjects"), true, "code for objects differs from code for strings or objects");
for (const name of ["First", "Second", "Third", "Fourth"])
    check(remarksOf("callee" + name), remarksOf("givenBooleans"), "callee" + name + " and a function that is only given booleans");
check(remarksOf("calleeLinked"), remarksOf("givenIntegers"), "calleeLinked and a function that is only given integers");
check(remarksOf("calleeUnnarrowed"), remarksOf("givenStringsOrObjects"), "calleeUnnarrowed and a function that is given strings or objects");
