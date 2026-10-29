//@ requireOptions("--maxRegExpStackSize=65536")

// The Yarr JIT frame for this pattern (480 KB) fits the stack of the mutator and not the stack of
// a DFG compiler thread, where the JIT code fails its stack check. The RegExp also has bytecode:
// one match below runs where the mutator has no room for the frame either. The interpreter cannot
// hold its first frame for this pattern in --maxRegExpStackSize=65536 and reports that as no match.
// DFG strength reduction must treat the failure on the compiler thread as "give up folding" whether
// the RegExp has bytecode or not. It must not fold the interpreter's answer: the mutator's JIT code
// matches this subject.
const termCount = 30000;
const regExp = new RegExp("^" + "a?".repeat(termCount) + "(b)?$");
const subject = "a";

function foldableTest() {
    return regExp.test(subject);
}
noInline(foldableTest);

function foldableExec() {
    return regExp.exec(subject) !== null;
}
noInline(foldableExec);

// The deepest frame that has room for the call makes it. The JIT code of the match has no room there.
function matchAtTheBottomOfTheStack() {
    try {
        return matchAtTheBottomOfTheStack();
    } catch {
        return regExp.test(subject);
    }
}

const expectedTest = foldableTest();
const expectedExec = foldableExec();
matchAtTheBottomOfTheStack();

let iterationsInOptimizedCode = 0;
for (let i = 0; i < testLoopCount && iterationsInOptimizedCode < 100; i++) {
    if (foldableTest() !== expectedTest)
        throw new Error("test() folded to " + !expectedTest + " at iteration " + i);
    if (foldableExec() !== expectedExec)
        throw new Error("exec() folded to " + (expectedExec ? "null" : "a match") + " at iteration " + i);
    if (numberOfDFGCompiles(foldableTest) && numberOfDFGCompiles(foldableExec))
        iterationsInOptimizedCode++;
}
