//@ requireOptions("--maxRegExpStackSize=65536")

// The Yarr JIT frame for this pattern (480 KB) fits the stack of the mutator and not the stack of
// a DFG compiler thread, where the JIT code fails its stack check. The RegExp also has bytecode:
// one match below runs where the mutator has no room for the frame either. So the compiler thread
// runs the interpreter, which cannot hold its first frame for this pattern in
// --maxRegExpStackSize=65536. It gives the search up, and reports that as no match. DFG strength
// reduction must treat that as "give up folding": the mutator's JIT code matches this subject.
const termCount = 30000;
const regExp = new RegExp("^" + "a?".repeat(termCount) + "(b)?$");
const subject = "a";
const subjects = [subject];

function foldableTest() {
    return regExp.test(subject);
}
noInline(foldableTest);

function foldableExec() {
    return regExp.exec(subject) !== null;
}
noInline(foldableExec);

// The DFG cannot fold these: the subject is not a constant. They are what the mutator answers.
function unfoldableTest(index) {
    return regExp.test(subjects[index]);
}
noInline(unfoldableTest);

function unfoldableExec(index) {
    return regExp.exec(subjects[index]) !== null;
}
noInline(unfoldableExec);

// The deepest frame that has room for the call makes it. The JIT code of the match has no room there.
function matchAtTheBottomOfTheStack() {
    try {
        return matchAtTheBottomOfTheStack();
    } catch {
        return regExp.test(subject);
    }
}

foldableTest();
foldableExec();
matchAtTheBottomOfTheStack();

let iterationsInOptimizedCode = 0;
for (let i = 0; i < testLoopCount && iterationsInOptimizedCode < 100; ++i) {
    const test = unfoldableTest(0);
    if (foldableTest() !== test)
        throw new Error("test() folded to " + !test + " at iteration " + i);
    const exec = unfoldableExec(0);
    if (foldableExec() !== exec)
        throw new Error("exec() folded to " + (exec ? "null" : "a match") + " at iteration " + i);
    if (numberOfDFGCompiles(foldableTest) && numberOfDFGCompiles(foldableExec))
        iterationsInOptimizedCode++;
}
