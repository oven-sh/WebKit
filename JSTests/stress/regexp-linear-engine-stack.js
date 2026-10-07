//@ runDefault("--useRegExpLinearEngine=1", "--useDollarVM=1")
//@ runDefault("--useRegExpLinearEngine=1", "--useDollarVM=1", "--useRegExpJIT=0")

function describe(value)
{
    return JSON.stringify(value, (key, entry) => entry === undefined ? "<undefined>" : entry);
}

// The compiler of the matcher refuses when the stack has too little room left for it. That is
// about the compile and not about the pattern, so the refusal lasts as long as the code of the
// RegExp does: once the code is deleted, the next compile asks the matcher again. The RegExp
// asks before it asks the JIT, which reorders the terms of a pattern ([a-c]x here), and the
// answers are the same whichever engine gives them.
function nested(depth, second = "x")
{
    return "[a-c]" + second + "(?:".repeat(depth) + "a" + ")".repeat(depth);
}

function refusalOf(regExp)
{
    return $vm.regExpMatchStatistics(regExp, "", 0).refusal;
}

// A RegExp of the same source is the same RegExp, and it has its code already. So every
// question to the compiler is a pattern of its own.
let probes = 0;
function refusalOfDepth(depth)
{
    return refusalOf(new RegExp(nested(depth, "y" + probes++)));
}

function recurse(frames, callback)
{
    return frames ? recurse(frames - 1, callback) : callback();
}

// The deepest nesting the compiler has the stack for, from 1024 frames down, to within 8 groups.
// A compile that has to be accepted below runs with more stack than that, and one that has to
// be refused runs from 2048 frames down at least.
let deepest = recurse(1024, () => {
    let accepted = 500;
    let tooDeep = 0;
    for (let depth = 1000; !tooDeep; depth *= 2) {
        let refusal = refusalOfDepth(depth);
        if (refusal === "none")
            accepted = depth;
        else if (refusal === "not enough stack")
            tooDeep = depth;
        else
            throw new Error(depth + " nested groups: " + refusal);
    }
    while (tooDeep - accepted > 8) {
        let depth = (accepted + tooDeep) >> 1;
        if (refusalOfDepth(depth) === "none")
            accepted = depth;
        else
            tooDeep = depth;
    }
    return accepted;
});

function answers(regExp)
{
    let statistics = $vm.regExpMatchStatistics(regExp, "axa", 0);
    let first = regExp.exec("axa");
    let second = regExp.exec("xaa");
    return { engine: statistics.engine, refusal: statistics.refusal, axa: first && first.index, xaa: second && second.index };
}

function expectAnswers(name, actual, expected)
{
    if (describe(actual) !== describe(expected))
        throw new Error(name + ": expected " + describe(expected) + " but got " + describe(actual));
}

let stackDependent = new RegExp(nested(deepest));

// The first compile, from so many frames down that the compiler has too little stack for a
// pattern this deep. The frames of a function are not of one size for the whole run, so each
// number of frames is asked with a pattern of its own, and the RegExp is compiled under the
// frames that refused that pattern, a few frames further down.
function firstCompileWithLittleStack()
{
    for (let frames = 2048; frames <= 65536; frames *= 2) {
        let result = recurse(frames, () => refusalOfDepth(deepest) === "not enough stack" ? recurse(16, () => answers(stackDependent)) : null);
        if (result)
            return result;
    }
    throw new Error("The compiler has the stack for " + deepest + " nested groups from 65536 frames down");
}
expectAnswers("first compile", firstCompileWithLittleStack(), { engine: "backtracking", refusal: "not enough stack", axa: 0, xaa: null });
// The refusal stays while the code does, whatever the stack is then.
expectAnswers("same code", answers(stackDependent), { engine: "backtracking", refusal: "not enough stack", axa: 0, xaa: null });

$vm.deleteAllCodeWhenIdle();
setTimeout(() => {
    expectAnswers("compile after the code is deleted", answers(stackDependent), { engine: "linear", refusal: "none", axa: 0, xaa: null });
}, 0);
