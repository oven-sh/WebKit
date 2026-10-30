//@ requireOptions("--maxRegExpStackSize=65536")
//@ defaultRun; run("flat-program-behind-jit-code", "--validateRegExpFlatProgram=true")

// The fold of a global replace makes one match after the other on the compiler thread, and builds the string
// as it goes. Each turn of the group takes a ParenContext from the stack. The first match ("abc") takes 2,
// and the JIT code makes it on a DFG compiler thread. The second takes 1200, which fit the stack of the
// mutator and not the 512 KB stack of a compiler thread: the JIT code fails there. The RegExp has bytecode,
// because one replace below runs where the mutator has no room either, and with --maxRegExpStackSize=65536
// the interpreter gives the second match up and reports no match. DFG strength reduction must not take that
// for the end of the matches: the string it folded had the first match replaced and the second left in.
// (The second run gives the bytecode a flat program and keeps the JIT: there the flat program gives the
// match up.)
const regExp = new RegExp("(" + "(x)?".repeat(20) + "[ab])*c", "g");
const subject = "abc-" + "ab".repeat(600) + "c";
const subjects = [subject];

function foldableReplace() {
    return subject.replace(regExp, "X");
}
noInline(foldableReplace);

function foldableReplaceWithEmpty() {
    return subject.replace(regExp, "");
}
noInline(foldableReplaceWithEmpty);

// The DFG cannot fold these: the subject is not a constant. They are what the mutator answers.
function unfoldableReplace(index) {
    return subjects[index].replace(regExp, "X");
}
noInline(unfoldableReplace);

function unfoldableReplaceWithEmpty(index) {
    return subjects[index].replace(regExp, "");
}
noInline(unfoldableReplaceWithEmpty);

// The deepest frame that has room for the call makes it. The JIT code of the matches has no room there.
function replaceAtTheBottomOfTheStack() {
    try {
        return replaceAtTheBottomOfTheStack();
    } catch {
        return subject.replace(regExp, "X");
    }
}

foldableReplace();
foldableReplaceWithEmpty();
replaceAtTheBottomOfTheStack();

let iterationsInOptimizedCode = 0;
for (let i = 0; i < testLoopCount && iterationsInOptimizedCode < 100; ++i) {
    const replaced = foldableReplace();
    if (replaced !== unfoldableReplace(0))
        throw new Error("replace(regExp, \"X\") folded to a string of length " + replaced.length + " that starts with " + replaced.slice(0, 8) + " at iteration " + i);
    const replacedWithEmpty = foldableReplaceWithEmpty();
    if (replacedWithEmpty !== unfoldableReplaceWithEmpty(0))
        throw new Error("replace(regExp, \"\") folded to a string of length " + replacedWithEmpty.length + " that starts with " + replacedWithEmpty.slice(0, 8) + " at iteration " + i);
    if (numberOfDFGCompiles(foldableReplace) && numberOfDFGCompiles(foldableReplaceWithEmpty))
        iterationsInOptimizedCode++;
}
