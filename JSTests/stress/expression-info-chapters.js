//@ runDefault
//@ runDefault("--useBytecodeOptimizer=1", "--expressionInfoWordsBetweenChapters=8")
//@ runDefault("--forceDebuggerBytecodeGeneration=1", "--expressionInfoWordsBetweenChapters=1")
//@ runBytecodeCache("--useBytecodeOptimizer=1")
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--expressionInfoWordsBetweenChapters=8")

// Expression info starts a chapter every 256 words, and a lookup of a source position decodes from the start of one
// chapter. So every throw of a function reports its own line, column and expression, and no lookup steps over more
// words than a chapter has. A generator and an async function have their bytecode rewritten after the expression info
// is recorded, which moves the instruction that each chapter starts at. With the option a chapter is short, and a
// function of a few cases has many.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

function shouldBeTrue(condition, message) {
    if (!condition)
        throw new Error(message);
}

function thrownBy(run) {
    try {
        run();
    } catch (error) {
        return error;
    }
    throw new Error("did not throw");
}

function rejectionOf(promise) {
    let reason;
    promise.then(() => { throw new Error("did not reject"); }, error => { reason = error; });
    drainMicrotasks();
    return reason;
}

function resultOf(promise) {
    let result;
    promise.then(value => { result = value; });
    drainMicrotasks();
    return result;
}

const wordsInAChapter = jscOptions().expressionInfoWordsBetweenChapters;
// 256 without the option.
shouldBeTrue([256, 8, 1].includes(wordsInAChapter), `${wordsInAChapter} words in a chapter`);
const wordsInAnEntry = 8; // at most: a chapter ends with the entry that fills it

// `error` is what case i throws, and `codeBlock` what case -1 returns: the code block with the cases, which for a
// generator or an async function is not the one of the function itself.
const kinds = {
    "function": {
        head: "function f(i, o)",
        selector: "i",
        receiver: "o",
        pause: "",
        error: (f, i) => thrownBy(() => f(i, {})),
        codeBlock: f => f(-1, {}),
    },
    "generator": {
        head: "function* f(i, o)",
        selector: "i",
        receiver: "o",
        pause: "yield 0;",
        error: (f, i) => thrownBy(() => { let iterator = f(i, {}); iterator.next(); iterator.next(); }),
        codeBlock: f => f(-1, {}).next().value,
    },
    // Behind the `yield` of a case nothing is in a register, so the rewrite saves nothing there and leaves less
    // bytecode than it found: what a chapter behind it starts at moves down.
    "generator that saves nothing": {
        head: "function* f()",
        selector: "yield",
        receiver: "this",
        pause: "yield 0;",
        error: (f, i) => thrownBy(() => { let iterator = f.call({}); iterator.next(); iterator.next(i); iterator.next(); }),
        codeBlock: f => { let iterator = f.call({}); iterator.next(); return iterator.next(-1).value; },
    },
    "async function": {
        head: "async function f(i, o)",
        selector: "i",
        receiver: "o",
        pause: "await 0;",
        error: (f, i) => rejectionOf(f(i, {})),
        codeBlock: f => resultOf(f(-1, {})),
    },
    "async generator": {
        head: "async function* f(i, o)",
        selector: "i",
        receiver: "o",
        pause: "yield 0; await 0;",
        error: (f, i) => { let iterator = f(i, {}); iterator.next(); return rejectionOf(iterator.next()); },
        codeBlock: f => resultOf(f(-1, {}).next()).value,
    },
};

// Case i throws a TypeError: an even case where it reads an index of undefined, an odd case where it calls
// undefined. `expected[i]` is its line, column and message: the column of an index is its `[`, of a call its `(`.
let functionCount = 0;
function makeFunction(kind, count) {
    let { head, selector, receiver, pause } = kinds[kind];
    // A name of its own: functions with the same text share their code, and with it the positions asked for before.
    let lines = [`(${head.replace("f(", `f${functionCount++}(`)} {`, `switch (${selector}) {`, "case -1: return $vm.codeBlockForFrame(0);"];
    let expected = [];
    for (let i = 0; i < count; i++) {
        let id = String(i).padStart(5, "0");
        let [site, divot, message] = i % 2
            ? [`${receiver}["n${id}"](1)`, "(1)", `${receiver}["n${id}"] is not a function. (In '${receiver}["n${id}"](1)', '${receiver}["n${id}"]' is undefined)`]
            : [`${receiver}.x[${i}]`, `[${i}]`, `undefined is not an object (evaluating '${receiver}.x[${i}]')`];
        let line = `case ${i}: ${pause} ${receiver}.a; ${receiver}.b; return ${site};`;
        lines.push(line);
        expected.push(`${lines.length}:${line.indexOf(divot) + 1} ${message}`);
    }
    lines.push("}", "})");
    // Not eval: a bytecode cache does not hold the code of an eval.
    return { f: loadString(lines.join("\n")), expected };
}

function check(kind, count) {
    let message = `${kind} with ${count} cases`;
    let { f, expected } = makeFunction(kind, count);
    let codeBlock = kinds[kind].codeBlock(f);

    // A chapter starts behind wordsInAChapter words or more of the one before it. The first chapter has no start.
    let { words, chapters } = $vm.expressionInfoStats(codeBlock);
    shouldBeTrue(words >= chapters * wordsInAChapter, `${message}: ${chapters} chapter starts in ${words} words`);
    shouldBeTrue(words < (chapters + 1) * (wordsInAChapter + wordsInAnEntry), `${message}: ${chapters} chapter starts in ${words} words`);

    for (let i = 0; i < count; i++) {
        let error = kinds[kind].error(f, i);
        shouldBe(`${error.line}:${error.column} ${error.message}`, expected[i], `${message}, case ${i}`);
    }

    // Every offset of the bytecode, not only the instructions that throw. Where that is a lot of decoding, every
    // eighth.
    let step = chapters && wordsInAChapter > 8 ? 8 : 1;
    let most = 0;
    for (let offset = 0; ; offset += step) {
        let decoded = $vm.expressionInfoWordsDecoded(codeBlock, offset);
        if (decoded === undefined)
            break;
        shouldBeTrue(decoded < wordsInAChapter + wordsInAnEntry, `${message}: the lookup of offset ${offset} decodes ${decoded} words`);
        most = Math.max(most, decoded);
    }
    // The lookup of the last entry of a chapter decodes all of that chapter, or all but that entry.
    if (step === 1)
        shouldBeTrue(most >= words / (chapters + 1) - wordsInAnEntry, `${message}: no lookup decodes more than ${most} of ${words} words in ${chapters + 1} chapters`);

    return { words, chapters };
}

for (let kind in kinds) {
    let { words, chapters } = check(kind, 24);
    // Without the option that is about a chapter of words: as many cases as make three.
    if (chapters < 2)
        ({ chapters } = check(kind, Math.ceil(3 * wordsInAChapter * 24 / words)));
    shouldBeTrue(chapters >= 2, `${kind}: ${chapters} chapter starts`);
}
