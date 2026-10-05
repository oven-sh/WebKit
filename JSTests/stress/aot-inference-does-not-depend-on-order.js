//@ runDefault("--compileMainScriptAheadOfTime=1", "--numberOfAOTCompilerThreads=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--numberOfAOTCompilerThreads=1", "--seedOfAOTInferenceOrderForTesting=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--numberOfAOTCompilerThreads=1", "--seedOfAOTInferenceOrderForTesting=2")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--numberOfAOTCompilerThreads=1", "--seedOfAOTInferenceOrderForTesting=3")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--numberOfAOTCompilerThreads=1", "--seedOfAOTInferenceOrderForTesting=4")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--numberOfAOTCompilerThreads=1", "--seedOfAOTInferenceOrderForTesting=5")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--numberOfAOTCompilerThreads=1", "--seedOfAOTInferenceOrderForTesting=6")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--numberOfAOTCompilerThreads=1", "--seedOfAOTInferenceOrderForTesting=7")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--numberOfAOTCompilerThreads=1", "--seedOfAOTInferenceOrderForTesting=8")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function remarksOf(name) {
    return aotRemarks(name).slice().sort().join(" ");
}

(function () {
    function lateFirst() { return middleFirst(7); }
    function earlyFirst() { return middleFirst("abc") + middleFirst({ a: 1 }); }
    function calleeFirst(s) { return s.length + s.charCodeAt(0); }
    function middleFirst(e) { if (typeof e === "string") return calleeFirst(e); return 0; }
    check(earlyFirst() + lateFirst(), 100, "the first copy");
})();

(function () {
    function lateSecond() { return middleSecond(7); }
    function earlySecond() { return middleSecond("abc") + middleSecond({ a: 1 }); }
    function calleeSecond(s) { return s.length + s.charCodeAt(0); }
    function middleSecond(e) { if (typeof e === "string") return calleeSecond(e); return 0; }
    check(earlySecond() + lateSecond(), 100, "the second copy");
})();

(function () {
    function lateThird() { return middleThird(7); }
    function earlyThird() { return middleThird("abc") + middleThird({ a: 1 }); }
    function calleeThird(s) { return s.length + s.charCodeAt(0); }
    function middleThird(e) { if (typeof e === "string") return calleeThird(e); return 0; }
    check(earlyThird() + lateThird(), 100, "the third copy");
})();

(function () {
    function lateFourth() { return middleFourth(7); }
    function earlyFourth() { return middleFourth("abc") + middleFourth({ a: 1 }); }
    function calleeFourth(s) { return s.length + s.charCodeAt(0); }
    function middleFourth(e) { if (typeof e === "string") return calleeFourth(e); return 0; }
    check(earlyFourth() + lateFourth(), 100, "the fourth copy");
})();

(function () {
    function lateFifth() { return middleFifth(7); }
    function earlyFifth() { return middleFifth("abc") + middleFifth({ a: 1 }); }
    function calleeFifth(s) { return s.length + s.charCodeAt(0); }
    function middleFifth(e) { if (typeof e === "string") return calleeFifth(e); return 0; }
    check(earlyFifth() + lateFifth(), 100, "the fifth copy");
})();

(function () {
    function lateSixth() { return middleSixth(7); }
    function earlySixth() { return middleSixth("abc") + middleSixth({ a: 1 }); }
    function calleeSixth(s) { return s.length + s.charCodeAt(0); }
    function middleSixth(e) { if (typeof e === "string") return calleeSixth(e); return 0; }
    check(earlySixth() + lateSixth(), 100, "the sixth copy");
})();

(function () {
    function callsWithStrings() { return passesStrings("abc"); }
    function givenStrings(s) { return s.length + s.charCodeAt(0); }
    function passesStrings(e) { return givenStrings(e); }
    check(callsWithStrings(), 100, "only strings");
})();

(function () {
    function callsWithStringsOrObjects() { return passesStringsOrObjects("abc") + passesStringsOrObjects({ length: 1, charCodeAt() { return 1; } }); }
    function givenStringsOrObjects(s) { return s.length + s.charCodeAt(0); }
    function passesStringsOrObjects(e) { return givenStringsOrObjects(e); }
    check(callsWithStringsOrObjects(), 102, "strings or objects");
})();

check(remarksOf("givenStrings") !== remarksOf("givenStringsOrObjects"), true, "code for strings differs from code for strings or objects");
for (const name of ["First", "Second", "Third", "Fourth", "Fifth", "Sixth"])
    check(remarksOf("callee" + name), remarksOf("givenStrings"), "callee" + name + " and a function that is only given strings");
