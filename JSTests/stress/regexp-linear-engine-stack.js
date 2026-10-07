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

// The deepest nesting the compiler has the stack for here, with the stack this frame leaves.
// These patterns are not the one below: a RegExp of the same source is the same RegExp, and it
// would have its code already.
let deepest = 500;
let tooDeep = 0;
for (let depth = 1000; !tooDeep; depth *= 2) {
    let refusal = refusalOf(new RegExp(nested(depth, "y")));
    if (refusal === "none")
        deepest = depth;
    else if (refusal === "not enough stack")
        tooDeep = depth;
    else
        throw new Error(depth + " nested groups: " + refusal);
}
while (tooDeep - deepest > deepest / 32) {
    let depth = (deepest + tooDeep) >> 1;
    if (refusalOf(new RegExp(nested(depth, "y"))) === "none")
        deepest = depth;
    else
        tooDeep = depth;
}

function recurse(frames, callback)
{
    return frames ? recurse(frames - 1, callback) : callback();
}

function fits(frames)
{
    try {
        recurse(frames, () => 1);
        return true;
    } catch {
        return false;
    }
}

let mostFrames = 1000;
while (fits(mostFrames * 2))
    mostFrames *= 2;
for (let step = mostFrames >> 1; step >= 64; step >>= 1) {
    if (fits(mostFrames + step))
        mostFrames += step;
}

function answers(regExp)
{
    let statistics = $vm.regExpMatchStatistics(regExp, "axa", 0);
    let first = regExp.exec("axa");
    let second = regExp.exec("xaa");
    return { engine: statistics.engine, refusal: statistics.refusal, axa: first && first.index, xaa: second && second.index };
}

let stackDependent = new RegExp(nested(deepest));
// With nine tenths of the stack in use, the compiler has too little of it for this pattern.
expectAnswers("first compile", recurse(Math.floor(mostFrames * 0.9), () => answers(stackDependent)), { engine: "backtracking", refusal: "not enough stack", axa: 0, xaa: null });
// The refusal stays while the code does, whatever the stack is then.
expectAnswers("same code", answers(stackDependent), { engine: "backtracking", refusal: "not enough stack", axa: 0, xaa: null });

function expectAnswers(name, actual, expected)
{
    if (describe(actual) !== describe(expected))
        throw new Error(name + ": expected " + describe(expected) + " but got " + describe(actual));
}

$vm.deleteAllCodeWhenIdle();
setTimeout(() => {
    expectAnswers("compile after the code is deleted", answers(stackDependent), { engine: "linear", refusal: "none", axa: 0, xaa: null });
}, 0);
