//@ skip if $buildType == "debug" or $asan
//@ requireOptions("--maxRegExpStackSize=1073741824")

// The RegExp has only bytecode: an alternative with a minimum length above INT_MAX keeps the pattern out of
// the Yarr JIT. The interpreter recurses once for each nested group that repeats. The stack of the mutator
// holds 4000 of them, and the 512 KB stack of a DFG compiler thread holds about 2000 to 2500: there the
// interpreter gives the search up, and reports that as no match. DFG strength reduction must not fold that
// answer.
// (--maxRegExpStackSize: the mutator needs more than the default for its backtracking state at this depth.
// A debug or ASan build has larger frames, and its mutator does not hold this depth either.)
const depth = 4000;
const wide = "\u0101";
let nested = wide;
for (let i = 0; i < depth; ++i)
    nested = "(?:x" + nested + "y)*";
const regExp = new RegExp("^(?:z{2147483648}|" + nested + ")$");
const subject = "x".repeat(depth) + wide + "y".repeat(depth);
const subjects = [subject];

// A match costs milliseconds, so the functions get hot through calls that make none.
function foldableTest(match) {
    return match ? regExp.test(subject) : false;
}
noInline(foldableTest);

function foldableExec(match) {
    return match ? regExp.exec(subject) !== null : false;
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

function check(iteration) {
    const test = unfoldableTest(0);
    if (foldableTest(true) !== test)
        throw new Error("test() folded to " + !test + " at iteration " + iteration);
    const exec = unfoldableExec(0);
    if (foldableExec(true) !== exec)
        throw new Error("exec() folded to " + (exec ? "null" : "a match") + " at iteration " + iteration);
}

if (!unfoldableTest(0) || !unfoldableExec(0))
    throw new Error("the mutator gave the match up");

// Most calls make no match. The first four and every 16th make one, so that the code of the DFG has the match
// in it, also where the DFG compiles after a few calls.
let iterationsInOptimizedCode = 0;
for (let i = 0; i < testLoopCount && iterationsInOptimizedCode < 64; ++i) {
    if (i % 16 && i >= 4)
        foldableTest(false), foldableExec(false);
    else
        check(i);
    if (numberOfDFGCompiles(foldableTest) && numberOfDFGCompiles(foldableExec))
        iterationsInOptimizedCode++;
}
check("the end");
