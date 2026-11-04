//@ runDefault
//@ runDefault("--useBytecodeOptimizer=1")
//@ runBytecodeCache
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1")

// Expression info is in chapters of 256 words. A lookup decodes from the start of a chapter, and one for a line and
// column from the cursor if it can: where the lookup before it ended. Every throw of a function has the position and
// the message of the same throw in a function of two cases, in whatever order they are asked for and wherever the
// chapters start. A generator and an async function have their bytecode rewritten after the expression info is
// recorded, which moves the instruction that each chapter starts at.

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

// `error` is what case i throws, and `codeBlock` what case -1 returns.
const kinds = {
    "function": {
        head: "function f(i, o)",
        pause: "",
        error: (f, i) => thrownBy(() => f(i, {})),
        codeBlock: f => f(-1, {}),
    },
    "generator": {
        head: "function* f(i, o)",
        pause: "yield 0;",
        error: (f, i) => thrownBy(() => { let iterator = f(i, {}); iterator.next(); iterator.next(); }),
        codeBlock: f => f(-1, {}).next().value,
    },
    "async function": {
        head: "async function f(i, o)",
        pause: "await 0;",
        error: (f, i) => rejectionOf(f(i, {})),
        codeBlock: f => resultOf(f(-1, {})),
    },
    "async generator": {
        head: "async function* f(i, o)",
        pause: "yield 0; await 0;",
        error: (f, i) => { let iterator = f(i, {}); iterator.next(); return rejectionOf(iterator.next()); },
        codeBlock: f => resultOf(f(-1, {}).next()).value,
    },
};

const fiveDigits = i => String(i).padStart(5, "0");

// Case i throws a TypeError on line i + 5: an even case where it reads a property of undefined, an odd case where it
// calls undefined. The statements ahead of the cases are `padding` words of expression info (`o.p;` is two and `o;`
// is one; the bytecode optimizer leaves about half), which move every chapter start by as much. Case -1 returns the
// code block with the cases, which for a generator or an async function is not the one of the function itself.
let functionCount = 0;
function makeFunction(kind, count, padding = 0) {
    // A name of its own: functions with the same text share their code, and with it the positions asked for before.
    let lines = [`(${kinds[kind].head.replace("f(", `f${functionCount++}(`)} {`, "o.p; ".repeat(padding >> 1) + "o; ".repeat(padding & 1), "switch (i) {"];
    lines.push("case -1: return $vm.codeBlockForFrame(0);");
    for (let i = 0; i < count; i++) {
        let site = i % 2 ? `return o.n${fiveDigits(i)}(i + 1);` : `return o.x.y${fiveDigits(i)};`;
        lines.push(`case ${i}:`.padEnd(12) + `${kinds[kind].pause} o.a; o.b; ${site}`);
    }
    lines.push("}", "})");
    return (0, eval)(lines.join("\n"));
}

function describe(error) {
    return `${error.line}:${error.column} ${error.message}`;
}

// What case i of any function of this kind throws, from what cases 0 and 1 of a function of two cases throw.
function expectations(kind) {
    let f = makeFunction(kind, 2);
    let references = [0, 1].map(i => kinds[kind].error(f, i));
    shouldBe(references[0].message, "undefined is not an object (evaluating 'o.x.y00000')", kind);
    shouldBe(references[1].message, "o.n00001 is not a function. (In 'o.n00001(i + 1)', 'o.n00001' is undefined)", kind);
    shouldBe(references[0].line, 5, kind);
    shouldBe(references[1].line, 6, kind);
    return i => {
        let reference = references[i % 2];
        return `${i + 5}:${reference.column} ${reference.message.replaceAll(fiveDigits(i % 2), fiveDigits(i))}`;
    };
}

function shuffled(count) {
    let indices = Array.from({ length: count }, (_, i) => i);
    let seed = 12345;
    for (let i = count - 1; i > 0; i--) {
        seed = (seed * 1103515245 + 12345) & 0x7fffffff;
        let j = seed % (i + 1);
        [indices[i], indices[j]] = [indices[j], indices[i]];
    }
    return indices;
}

function alternating(first, second) {
    let indices = [];
    for (let i = 0; i < first.length || i < second.length; i++) {
        if (i < first.length)
            indices.push(first[i]);
        if (i < second.length)
            indices.push(second[i]);
    }
    return indices;
}

const ascending = count => Array.from({ length: count }, (_, i) => i);
const descending = count => ascending(count).reverse();
const orders = {
    ascending,
    descending,
    shuffled,
    // From the two ends to the middle.
    sawtooth: count => alternating(ascending(count >> 1), descending(count).slice(0, count - (count >> 1))),
    // Two ascending runs, half a function apart.
    interleaved: count => alternating(ascending(count >> 1), ascending(count).slice(count >> 1)),
};

const wordsInAChapter = 256;
const wordsInAnEntry = 8; // at most: a chapter ends with the entry that fills it

function checkShape(codeBlock, message) {
    let { words, chapters, cursor } = $vm.expressionInfoStats(codeBlock);
    shouldBeTrue(words >= chapters * wordsInAChapter, `${message}: ${chapters} chapter starts in ${words} words`);
    shouldBeTrue(words < (chapters + 1) * (wordsInAChapter + wordsInAnEntry), `${message}: ${chapters} chapter starts in ${words} words`);
    // Only expression info with a chapter start has a cursor.
    shouldBe(cursor < 0, !chapters, `${message}: cursor ${cursor} with ${chapters} chapter starts`);
    return { words, chapters, cursor };
}

function checkPositions(kind, f, count, order, expected, message) {
    let codeBlock = kinds[kind].codeBlock(f);
    let before = checkShape(codeBlock, message);
    // Nothing asked for a line and column yet.
    shouldBe(before.cursor, before.chapters ? 0 : -1, message);
    let cursor = 0;
    for (let i of orders[order](count)) {
        shouldBe(describe(kinds[kind].error(f, i)), expected(i), `${message}, case ${i}`);
        if (!before.chapters)
            continue;
        // Each lookup for a line and column leaves the cursor behind the entry that it found.
        let next = $vm.expressionInfoStats(codeBlock).cursor;
        shouldBeTrue(next > 0 && next <= before.words, `${message}, case ${i}: cursor ${next} of ${before.words} words`);
        if (order === "ascending" || (order === "descending" && !cursor))
            shouldBeTrue(next > cursor, `${message}, case ${i}: cursor ${cursor} then ${next}`);
        else if (order === "descending")
            shouldBeTrue(next < cursor, `${message}, case ${i}: cursor ${cursor} then ${next}`);
        cursor = next;
    }
    let after = checkShape(codeBlock, message);
    shouldBe(after.words, before.words, message);
    shouldBe(after.chapters, before.chapters, message);
    return after;
}

// Every offset of the bytecode, which is more than the instructions that throw: some are in front of the first entry of
// the expression info, and most have no entry of their own. A stack trace finds the line and column of an offset from
// the cursor or from the start of its chapter, and a compiler thread from the start of its chapter.
function checkEveryOffset(kind, count, order, message) {
    let codeBlock = kinds[kind].codeBlock(makeFunction(kind, count));
    let size = 0;
    while ($vm.lineAndColumnForBytecodeOffset(codeBlock, size, true) !== undefined)
        size++;
    shouldBeTrue(size > count, `${message}: ${size} bytes of bytecode`);
    for (let offset of orders[order](size))
        shouldBe($vm.lineAndColumnForBytecodeOffset(codeBlock, offset), $vm.lineAndColumnForBytecodeOffset(codeBlock, offset, true), `${message}, offset ${offset}`);
}

for (let kind in kinds) {
    let expected = expectations(kind);

    // Up to one chapter of words and just above it, one word of padding more at a time. `chapters` counts the chapter
    // starts, which the first chapter has none of.
    let withoutChapterStart = 0;
    let withChapterStart = 0;
    for (let padding = 0; withChapterStart < 8; padding++) {
        shouldBeTrue(padding < 1000, `${kind}: no function with a chapter start`);
        let count = 24;
        let message = `${kind} with ${count} cases and ${padding} words of padding`;
        let { chapters } = checkPositions(kind, makeFunction(kind, count, padding), count, "ascending", expected, message);
        shouldBeTrue(chapters <= 1, `${message}: ${chapters} chapter starts`);
        if (chapters)
            withChapterStart++;
        else
            withoutChapterStart++;
    }
    shouldBeTrue(withoutChapterStart >= 8, `${kind}: ${withoutChapterStart} functions without a chapter start`);

    // Many chapters, in every order.
    for (let order in orders) {
        let count = 500;
        let message = `${kind} with ${count} cases, ${order}`;
        let { words, chapters, cursor } = checkPositions(kind, makeFunction(kind, count), count, order, expected, message);
        shouldBeTrue(chapters >= 8, `${message}: ${chapters} chapter starts in ${words} words`);
        if (order === "ascending")
            shouldBeTrue(cursor > words - wordsInAChapter && cursor <= words, `${message}: cursor ${cursor} of ${words} words`);
    }

    // As few cases as make two chapter starts: the bytecode optimizer leaves about half of the expression info.
    let count = 40;
    while ($vm.expressionInfoStats(kinds[kind].codeBlock(makeFunction(kind, count))).chapters < 2)
        count += 40;
    for (let order in orders)
        checkEveryOffset(kind, count, order, `${kind} with ${count} cases, every offset, ${order}`);

    // Many chapters, each starting at every distance from a case.
    for (let padding = 1; padding <= 16; padding++) {
        let count = 120;
        for (let order of ["ascending", "interleaved"]) {
            let message = `${kind} with ${count} cases and ${padding} words of padding, ${order}`;
            checkPositions(kind, makeFunction(kind, count, padding), count, order, expected, message);
        }
    }
}

// More than one chapter in code that a bytecode cache holds: this file's own.
let lineOfCached = new Error().line + 1;
function cached(i, o) {
    switch (i) {
    case 0:  o.a; o.b; o.c; o.d; return o.x.y00000;
    case 1:  o.a; o.b; o.c; o.d; return o.x.y00001;
    case 2:  o.a; o.b; o.c; o.d; return o.x.y00002;
    case 3:  o.a; o.b; o.c; o.d; return o.x.y00003;
    case 4:  o.a; o.b; o.c; o.d; return o.x.y00004;
    case 5:  o.a; o.b; o.c; o.d; return o.x.y00005;
    case 6:  o.a; o.b; o.c; o.d; return o.x.y00006;
    case 7:  o.a; o.b; o.c; o.d; return o.x.y00007;
    case 8:  o.a; o.b; o.c; o.d; return o.x.y00008;
    case 9:  o.a; o.b; o.c; o.d; return o.x.y00009;
    case 10: o.a; o.b; o.c; o.d; return o.x.y00010;
    case 11: o.a; o.b; o.c; o.d; return o.x.y00011;
    case 12: o.a; o.b; o.c; o.d; return o.x.y00012;
    case 13: o.a; o.b; o.c; o.d; return o.x.y00013;
    case 14: o.a; o.b; o.c; o.d; return o.x.y00014;
    case 15: o.a; o.b; o.c; o.d; return o.x.y00015;
    case 16: o.a; o.b; o.c; o.d; return o.x.y00016;
    case 17: o.a; o.b; o.c; o.d; return o.x.y00017;
    case 18: o.a; o.b; o.c; o.d; return o.x.y00018;
    case 19: o.a; o.b; o.c; o.d; return o.x.y00019;
    case 20: o.a; o.b; o.c; o.d; return o.x.y00020;
    case 21: o.a; o.b; o.c; o.d; return o.x.y00021;
    case 22: o.a; o.b; o.c; o.d; return o.x.y00022;
    case 23: o.a; o.b; o.c; o.d; return o.x.y00023;
    case 24: o.a; o.b; o.c; o.d; return o.x.y00024;
    case 25: o.a; o.b; o.c; o.d; return o.x.y00025;
    case 26: o.a; o.b; o.c; o.d; return o.x.y00026;
    case 27: o.a; o.b; o.c; o.d; return o.x.y00027;
    case 28: o.a; o.b; o.c; o.d; return o.x.y00028;
    case 29: o.a; o.b; o.c; o.d; return o.x.y00029;
    case 30: o.a; o.b; o.c; o.d; return o.x.y00030;
    case 31: o.a; o.b; o.c; o.d; return o.x.y00031;
    case 32: o.a; o.b; o.c; o.d; return o.x.y00032;
    case 33: o.a; o.b; o.c; o.d; return o.x.y00033;
    case 34: o.a; o.b; o.c; o.d; return o.x.y00034;
    case 35: o.a; o.b; o.c; o.d; return o.x.y00035;
    case 36: o.a; o.b; o.c; o.d; return o.x.y00036;
    case 37: o.a; o.b; o.c; o.d; return o.x.y00037;
    case 38: o.a; o.b; o.c; o.d; return o.x.y00038;
    case 39: o.a; o.b; o.c; o.d; return o.x.y00039;
    case 40: o.a; o.b; o.c; o.d; return o.x.y00040;
    case 41: o.a; o.b; o.c; o.d; return o.x.y00041;
    case 42: o.a; o.b; o.c; o.d; return o.x.y00042;
    case 43: o.a; o.b; o.c; o.d; return o.x.y00043;
    case 44: o.a; o.b; o.c; o.d; return o.x.y00044;
    case 45: o.a; o.b; o.c; o.d; return o.x.y00045;
    case 46: o.a; o.b; o.c; o.d; return o.x.y00046;
    case 47: o.a; o.b; o.c; o.d; return o.x.y00047;
    case 48: o.a; o.b; o.c; o.d; return o.x.y00048;
    case 49: o.a; o.b; o.c; o.d; return o.x.y00049;
    case 50: o.a; o.b; o.c; o.d; return o.x.y00050;
    case 51: o.a; o.b; o.c; o.d; return o.x.y00051;
    case 52: o.a; o.b; o.c; o.d; return o.x.y00052;
    case 53: o.a; o.b; o.c; o.d; return o.x.y00053;
    case 54: o.a; o.b; o.c; o.d; return o.x.y00054;
    case 55: o.a; o.b; o.c; o.d; return o.x.y00055;
    case 56: o.a; o.b; o.c; o.d; return o.x.y00056;
    case 57: o.a; o.b; o.c; o.d; return o.x.y00057;
    case 58: o.a; o.b; o.c; o.d; return o.x.y00058;
    case 59: o.a; o.b; o.c; o.d; return o.x.y00059;
    case 60: o.a; o.b; o.c; o.d; return o.x.y00060;
    case 61: o.a; o.b; o.c; o.d; return o.x.y00061;
    case 62: o.a; o.b; o.c; o.d; return o.x.y00062;
    case 63: o.a; o.b; o.c; o.d; return o.x.y00063;
    }
}

{
    shouldBe(cached(-1, {}), undefined, "cached");
    let before = $vm.expressionInfoStats(cached);
    shouldBeTrue(before.chapters >= 1, `cached: ${before.chapters} chapter starts in ${before.words} words`);
    shouldBe(before.cursor, 0, "cached");
    let column = cached.toString().split("\n")[2].indexOf(".y") + 1;
    for (let order of ["ascending", "shuffled"]) {
        for (let i of orders[order](64)) {
            let expected = `${lineOfCached + 2 + i}:${column} undefined is not an object (evaluating 'o.x.y${fiveDigits(i)}')`;
            shouldBe(describe(thrownBy(() => cached(i, {}))), expected, `cached, ${order}, case ${i}`);
        }
    }
    let after = checkShape(cached, "cached");
    shouldBeTrue(after.cursor > wordsInAChapter, `cached: cursor ${after.cursor} of ${after.words} words`);
}
