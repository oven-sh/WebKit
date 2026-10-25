// The cleanup block of a for-of loop (IteratorClose) is entered by an exception, a return, or a jump to an outer label, never by normal
// completion. These check that what the loop computed is intact on every way out of it, and that the iterator is closed when it should be.

function shouldBe(actual, expected, what) {
    if (JSON.stringify(actual) !== JSON.stringify(expected))
        throw new Error(what + ": expected " + JSON.stringify(expected) + " but got " + JSON.stringify(actual));
}

let log = [];
function iterable(values, options = {}) {
    return {
        [Symbol.iterator]() {
            let i = 0;
            return {
                next() {
                    if (options.throwAt === i)
                        throw new Error("next " + i);
                    return i < values.length ? { done: false, value: values[i++] } : { done: true, value: undefined };
                },
                return() {
                    log.push("closed");
                    if (options.closeThrows)
                        throw new Error("close");
                    return options.closeReturns === undefined ? {} : options.closeReturns;
                },
            };
        },
    };
}
function closes(f) {
    log = [];
    let result;
    try { result = f(); } catch (e) { result = "threw " + e.message; }
    return [result, log.length];
}

function sumUntilThrow(it, limit) {
    let n = 0, count = 0, last = "none";
    try {
        for (const x of it) {
            count++;
            last = "x" + x;
            if (x === limit)
                throw new Error("body " + x);
            n += x;
        }
    } catch (e) {
        return [n, count, last, e.message];
    }
    return [n, count, last, "finished"];
}

function throwEscapes(it, limit, out) {
    let n = 0;
    for (const x of it) {
        out.seen = ++n;
        if (x === limit)
            throw new Error("escaped " + x);
    }
    return n;
}

function returnsFromLoop(it, wanted) {
    let index = 0;
    for (const x of it) {
        if (x === wanted)
            return [index, x];
        index++;
    }
    return [index, "absent"];
}

function breaks(it, wanted) {
    let index = 0, found = false;
    for (const x of it) {
        if (x === wanted) {
            found = true;
            break;
        }
        index++;
    }
    return [index, found];
}

function nested(outer, make, stopAt) {
    let total = 0, outerCount = 0, innerCount = 0;
    for (const a of outer) {
        outerCount++;
        for (const b of make(a)) {
            innerCount++;
            if (b === stopAt)
                break;
            total += a * b;
        }
    }
    return [total, outerCount, innerCount];
}

function continueOuter(outer, make, skip) {
    let total = 0, afterInner = 0;
    outerLoop:
    for (const a of outer) {
        for (const b of make(a)) {
            if (b === skip)
                continue outerLoop;
            total += b;
        }
        afterInner++;
    }
    return [total, afterInner];
}

function breakOuter(outer, make, stop) {
    let total = 0, steps = 0;
    outerLoop:
    for (const a of outer) {
        for (const b of make(a)) {
            steps++;
            if (b === stop)
                break outerLoop;
            total += b;
        }
    }
    return [total, steps];
}

function nestedThrowCaughtBetween(outer, make, bad) {
    let total = 0, caught = 0, outerCount = 0;
    for (const a of outer) {
        outerCount++;
        try {
            for (const b of make(a)) {
                if (b === bad)
                    throw new Error("inner");
                total += b;
            }
        } catch {
            caught++;
        }
    }
    return [total, caught, outerCount];
}

function withFinally(it, limit) {
    let n = 0, cleanups = 0;
    try {
        for (const x of it) {
            try {
                if (x === limit)
                    return [n, cleanups, "returned"];
                n += x;
            } finally {
                cleanups++;
            }
        }
    } finally {
        cleanups += 100;
    }
    return [n, cleanups, "finished"];
}

function* generator(it) {
    let n = 0;
    try {
        for (const x of it) {
            n += x;
            yield n;
        }
    } finally {
        log.push("generator saw " + n);
    }
    return n;
}

async function awaits(values, limit) {
    let n = 0, count = 0;
    try {
        for await (const x of values) {
            count++;
            if (x === limit)
                throw new Error("async body");
            n += x;
        }
    } catch (e) {
        return [n, count, e.message];
    }
    return [n, count, "finished"];
}

const arrays = [[1, 2, 3, 4], [1.5, 2, 3, 4], ["1", 2, 3, 4]];
for (let i = 0; i < testLoopCount; i++) {
    shouldBe(sumUntilThrow([1, 2, 3, 4], 3), [3, 3, "x3", "body 3"], "array, throw");
    shouldBe(sumUntilThrow([1, 2, 3, 4], 9), [10, 4, "x4", "finished"], "array, no throw");
    shouldBe(closes(() => sumUntilThrow(iterable([1, 2, 3, 4]), 3)), [[3, 3, "x3", "body 3"], 1], "iterable, throw");
    shouldBe(closes(() => sumUntilThrow(iterable([1, 2, 3, 4]), 9)), [[10, 4, "x4", "finished"], 0], "iterable, no throw");
    shouldBe(closes(() => sumUntilThrow(iterable([1, 2, 3, 4], { closeThrows: true }), 2)), [[1, 2, "x2", "body 2"], 1], "the exception of the body wins");
    shouldBe(closes(() => sumUntilThrow(iterable([1, 2, 3, 4], { throwAt: 2 }), 9)), [[3, 2, "x2", "next 2"], 0], "next throws: not closed");

    let out = { seen: 0 };
    shouldBe(closes(() => throwEscapes(iterable([5, 6, 7]), 6, out)), ["threw escaped 6", 1], "escapes");
    shouldBe(out.seen, 2, "escapes, seen");
    shouldBe(closes(() => throwEscapes([5, 6, 7], 7, out)), ["threw escaped 7", 0], "escapes an array");
    shouldBe(out.seen, 3, "escapes an array, seen");

    shouldBe(closes(() => returnsFromLoop(iterable([5, 6, 7]), 6)), [[1, 6], 1], "return");
    shouldBe(closes(() => returnsFromLoop(iterable([5, 6, 7]), 8)), [[3, "absent"], 0], "no return");
    shouldBe(closes(() => returnsFromLoop(iterable([5, 6, 7], { closeThrows: true }), 7)), ["threw close", 1], "return, close throws");
    shouldBe(closes(() => returnsFromLoop(iterable([5, 6, 7], { closeReturns: 1 }), 7))[1], 1, "return, close returns a primitive");
    shouldBe(returnsFromLoop(arrays[i % 3], 3), [2, 3], "return from an array");

    shouldBe(closes(() => breaks(iterable([5, 6, 7]), 7)), [[2, true], 1], "break");
    shouldBe(closes(() => breaks(iterable([5, 6, 7]), 8)), [[3, false], 0], "no break");
    shouldBe(breaks([5, 6, 7], 6), [1, true], "break from an array");

    shouldBe(nested([1, 2, 3], a => [a, a + 1, 9, a + 2], 9), [1 * 1 + 1 * 2 + 2 * 2 + 2 * 3 + 3 * 3 + 3 * 4, 3, 9], "nested arrays");
    shouldBe(closes(() => nested(iterable([1, 2]), a => iterable([a, 9, a]), 9)), [[1 + 4, 2, 4], 2], "nested iterables");
    shouldBe(closes(() => continueOuter(iterable([1, 2, 3]), a => iterable([a, a === 2 ? 0 : a, a]), 0)), [[3 + 2 + 9, 2], 1], "continue outer");
    shouldBe(continueOuter([1, 2, 3], a => [a, a === 2 ? 0 : a, a], 0), [3 + 2 + 9, 2], "continue outer, arrays");
    shouldBe(closes(() => breakOuter(iterable([1, 2, 3]), a => iterable([a, a === 2 ? 0 : a, a]), 0)), [[3 + 2, 5], 2], "break outer");
    shouldBe(breakOuter([1, 2, 3], a => [a, a === 2 ? 0 : a, a], 0), [3 + 2, 5], "break outer, arrays");
    shouldBe(closes(() => nestedThrowCaughtBetween(iterable([1, 2, 3]), a => iterable([a, a === 2 ? 0 : a, a]), 0)), [[3 + 2 + 9, 1, 3], 1], "caught between");
    shouldBe(nestedThrowCaughtBetween([1, 2, 3], a => [a, a === 2 ? 0 : a, a], 0), [3 + 2 + 9, 1, 3], "caught between, arrays");

    shouldBe(closes(() => withFinally(iterable([1, 2, 3]), 3)), [[3, 2, "returned"], 1], "finally, return"); // (The result is made before the finally blocks run.)
    shouldBe(closes(() => withFinally(iterable([1, 2, 3]), 9)), [[6, 103, "finished"], 0], "finally, no return");
    shouldBe(withFinally([1, 2, 3], 2), [1, 1, "returned"], "finally, return from an array");

    log = [];
    let g = generator(iterable([1, 2, 3]));
    shouldBe([g.next().value, g.next().value, g.return(7).value, g.next().done], [1, 3, 7, true], "generator return");
    shouldBe(log, ["closed", "generator saw 3"], "generator return closes");
    log = [];
    g = generator([1, 2, 3]);
    g.next();
    let message;
    try { g.throw(new Error("injected")); } catch (e) { message = e.message; }
    shouldBe([message, log], ["injected", ["generator saw 1"]], "generator throw");
}

let settled = 0;
awaits([1, Promise.resolve(2), 3], 3).then(r => { shouldBe(r, [3, 3, "async body"], "for await, throw"); settled++; });
awaits([1, Promise.resolve(2), 3], 9).then(r => { shouldBe(r, [6, 3, "finished"], "for await"); settled++; });
awaits([1, Promise.reject(new Error("rejected")), 3], 9).then(r => { shouldBe(r, [1, 1, "rejected"], "for await, rejection"); settled++; });
drainMicrotasks();
shouldBe(settled, 3, "for await settled");
