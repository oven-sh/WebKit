//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "-m")
function check(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}
const errors = [];
function startsAsyncFunctions() {
    async function innerNameThatNothingElseUses() {
        await 1;
        errors.push(new Error("after an await"));
    }
    async function outerNameThatNothingElseUses() {
        await innerNameThatNothingElseUses();
    }
    outerNameThatNothingElseUses();
}
startsAsyncFunctions();
drainMicrotasks();
gc();
fullGC();
check(errors.length, 1, "errors");
check(errors[0].stack.includes("innerNameThatNothingElseUses"), true, "the name of the function that made the error, in a trace that the collector made text of");
check(errors[0].stack.includes("outerNameThatNothingElseUses"), true, "the name of the function that awaits it, in a trace that the collector made text of");
