// What a generator or an async function has to get right about its locals when it suspends and resumes. Each case is about
// one way for a local to be in the wrong place: still in the generator frame when it is read, or not yet there when the
// function suspends or throws.

function shouldBe(actual, expected)
{
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

function opaque(value) { return value; }
noInline(opaque);

function throwIf(condition, value = "thrown")
{
    if (condition)
        throw value;
}
noInline(throwIf);

// steps is a string of n (next), t (throw) and r (return). Step number i passes 100 + i.
function drive(generator, steps)
{
    let trace = [];
    for (let i = 0; i < steps.length; i++) {
        try {
            let method = { n: "next", t: "throw", r: "return" }[steps[i]];
            let result = generator[method](100 + i);
            trace.push((result.done ? "done:" : "") + result.value);
        } catch (error) {
            trace.push("threw:" + error);
        }
    }
    return trace.join(" ");
}

function settle(promise)
{
    let outcome;
    promise.then(value => { outcome = "value:" + value; }, error => { outcome = "threw:" + error; });
    drainMicrotasks();
    return outcome;
}

let cases = [];
function test(name, body) { cases.push({ name, body }); }

test("only a handler reads the local", () => {
    function* inCatch() {
        let a = opaque(1);
        try {
            yield 1;
            yield 2;
            yield 3;
        } catch (error) {
            return a + error;
        }
    }
    shouldBe(drive(inCatch(), "nt"), "1 done:102");
    shouldBe(drive(inCatch(), "nnt"), "1 2 done:103");
    shouldBe(drive(inCatch(), "nnnt"), "1 2 3 done:104");

    function* inFinally(log) {
        let a = opaque(1);
        try {
            yield 1;
            yield 2;
        } finally {
            log.push(a);
        }
    }
    for (let steps of ["nr", "nnr", "nt", "nnt", "nnn"]) {
        let log = [];
        drive(inFinally(log), steps);
        shouldBe(log.join(), "1");
    }
});

// A resume point inside a try block can get to the handler by an ordinary jump, since it may have been resumed with throw().
// One in front of the try block only gets there if something in the block throws.
test("only a handler reads the local, and the try block does not suspend", () => {
    function* g(fail) {
        let a = opaque(1);
        let b = opaque(2);
        yield 0;
        try {
            throwIf(fail);
            return "no";
        } catch {
            return a + b;
        }
    }
    shouldBe(drive(g(true), "nn"), "0 done:3");
    shouldBe(drive(g(false), "nn"), "0 done:no");

    function* nested(fail) {
        let a = opaque(1);
        try {
            yield 0;
            try {
                throwIf(fail);
            } finally {
                a += 10;
            }
            yield 1;
        } catch {
            a += 100;
        }
        return a;
    }
    shouldBe(drive(nested(true), "nn"), "0 done:111");
    shouldBe(drive(nested(false), "nnn"), "0 1 done:11");
});

test("a local that is saved has to have been restored", () => {
    function* g(write) {
        let a = opaque(1);
        yield 0;
        if (write)
            a = 2;
        yield 1;
        yield 2;
        return a;
    }
    shouldBe(drive(g(false), "nnnn"), "0 1 2 done:1");
    shouldBe(drive(g(true), "nnnn"), "0 1 2 done:2");
});

test("a suspend point after a handler saves what the try block wrote", () => {
    function* g(fail) {
        let a = opaque(1);
        yield 0;
        try {
            a = 2;
            throwIf(fail);
            a = 3;
        } catch { }
        yield 1;
        return a;
    }
    shouldBe(drive(g(true), "nnn"), "0 1 done:2");
    shouldBe(drive(g(false), "nnn"), "0 1 done:3");
});

test("a value that nothing but the handler reads", () => {
    function* g(failAt) {
        let seen;
        {
            let t = opaque(0);
            try {
                yield "a";
                t = 5;
                throwIf(failAt === 0);
                t = 6;
                yield "b";
                throwIf(failAt === 1);
                t = 7;
                throwIf(failAt === 2);
                t = 8;
            } catch {
                seen = t;
            }
        }
        yield seen;
    }
    shouldBe(drive(g(0), "nn"), "a 5");
    shouldBe(drive(g(1), "nnn"), "a b 6");
    shouldBe(drive(g(2), "nnn"), "a b 7");
    shouldBe(drive(g(3), "nnn"), "a b undefined");
    shouldBe(drive(g(3), "nt"), "a 0");
    shouldBe(drive(g(3), "nnt"), "a b 6");
});

test("a definition that reaches a read other paths reach by resuming", () => {
    function* g(suspend) {
        let a;
        if (suspend) {
            a = opaque(1);
            yield 0;
        } else
            a = opaque(2);
        let result = a + 10;
        yield result;
    }
    shouldBe(drive(g(true), "nn"), "0 11");
    shouldBe(drive(g(false), "n"), "12");
});

test("a handler cannot trust registers", () => {
    function* g(failEarly) {
        let a = opaque(1);
        let b = opaque(2);
        try {
            throwIf(failEarly);
            yield 0;
            throwIf(true);
        } catch {
            return a + b;
        }
    }
    shouldBe(drive(g(true), "n"), "done:3");
    shouldBe(drive(g(false), "nn"), "0 done:3");
});

test("a value from an earlier iteration", () => {
    function* g(count) {
        let previous = -1;
        let seen = [];
        for (let i = 0; i < count; i++) {
            seen.push(previous);
            yield i;
            if (i & 1)
                previous = i;
            yield -i;
        }
        return seen.join();
    }
    shouldBe(drive(g(5), "nnnnnnnnnnn"), "0 0 1 -1 2 -2 3 -3 4 -4 done:-1,-1,1,1,3");

    function* rotate(count) {
        let a = 1, b = 2, c = 3;
        for (let i = 0; i < count; i++) {
            let t = a;
            a = b;
            yield a;
            b = c;
            c = t;
        }
        return `${a}${b}${c}`;
    }
    shouldBe(drive(rotate(4), "nnnnn"), "2 3 1 2 done:231");
});

test("loops that do not suspend, between suspend points", () => {
    function* g(count, failAt) {
        let sum = 0;
        let product = 1;
        try {
            yield "start";
            for (let i = 0; i < count; i++) {
                sum += i;
                throwIf(i === failAt);
                product = (product * 3) % 1000;
            }
            yield sum;
            for (let i = 0; i < count; i++)
                sum -= product & i;
            yield sum;
        } catch {
            yield `caught ${sum} ${product}`;
        } finally {
            yield `finally ${sum} ${product}`;
        }
        return sum + product;
    }
    shouldBe(drive(g(10, -1), "nnnnn"), "start 45 40 finally 40 49 done:89");
    shouldBe(drive(g(10, 4), "nnnn"), "start caught 10 81 finally 10 81 done:91");
});

test("loops that suspend only sometimes", () => {
    function* g(count, suspendAt) {
        let sum = 0;
        for (let i = 0; i < count; i++) {
            if (i === suspendAt)
                sum += yield sum;
            sum += i;
        }
        return sum;
    }
    shouldBe(drive(g(6, 3), "nn"), "3 done:116");
    shouldBe(drive(g(6, 9), "n"), "done:15");
});

test("leaving through finally blocks", () => {
    function* g(how, log) {
        let a = opaque("a"), b = opaque("b"), c = opaque("c");
        outer: for (let i = 0; i < 2; i++) {
            try {
                try {
                    yield `in${i}`;
                    if (how === "break")
                        break outer;
                    if (how === "continue")
                        continue outer;
                    if (how === "return")
                        return "returned";
                    if (how === "throw")
                        throw "thrown";
                } finally {
                    log.push(a + i);
                    yield `inner${i}`;
                    log.push(b + i);
                }
            } finally {
                log.push(c + i);
                yield `outer${i}`;
                log.push(a + b + c);
            }
        }
        return "end";
    }
    function run(how, steps) {
        let log = [];
        return drive(g(how, log), steps) + " | " + log.join();
    }
    shouldBe(run("break", "nnnn"), "in0 inner0 outer0 done:end | a0,b0,c0,abc");
    shouldBe(run("continue", "nnnnnnn"), "in0 inner0 outer0 in1 inner1 outer1 done:end | a0,b0,c0,abc,a1,b1,c1,abc");
    shouldBe(run("return", "nnnn"), "in0 inner0 outer0 done:returned | a0,b0,c0,abc");
    shouldBe(run("throw", "nnnn"), "in0 inner0 outer0 threw:thrown | a0,b0,c0,abc");
    shouldBe(run("none", "nrnn"), "in0 inner0 outer0 done:101 | a0,b0,c0,abc");
    shouldBe(run("none", "ntnn"), "in0 inner0 outer0 threw:101 | a0,b0,c0,abc");
    shouldBe(run("none", "nnrn"), "in0 inner0 outer0 done:102 | a0,c0,abc");
    shouldBe(run("none", "nntn"), "in0 inner0 outer0 threw:102 | a0,c0,abc");
});

test("resuming with throw and return", () => {
    function* g() {
        let a = opaque(1);
        let received = [];
        for (;;) {
            try {
                received.push(yield a);
                a += 1;
            } catch (error) {
                received.push("c" + error);
                a += 10;
            } finally {
                a += 100;
                if (a > 400)
                    return received.join() + ":" + a;
            }
        }
    }
    shouldBe(drive(g(), "nntnrn"), "1 102 212 313 done:101,c102,103:413 done:undefined");
});

test("closing iterators", () => {
    function iterable(count, log) {
        let i = 0;
        return {
            [Symbol.iterator]() { return this; },
            next() { return { done: i >= count, value: i++ }; },
            return() { log.push("closed at " + i); return { }; },
        };
    }
    function* g(log, how) {
        let a = opaque("a");
        for (let x of iterable(3, log)) {
            for (let y of iterable(3, log)) {
                yield `${x}${y}`;
                if (how === "break" && y === 1)
                    break;
                throwIf(how === "throw" && x === 1);
            }
            log.push(a + x);
        }
        return a;
    }
    function run(how, steps) {
        let log = [];
        return drive(g(log, how), steps) + " | " + log.join();
    }
    shouldBe(run("break", "nnnnnnn"), "00 01 10 11 20 21 done:a | closed at 2,a0,closed at 2,a1,closed at 2,a2");
    shouldBe(run("throw", "nnnnn"), "00 01 02 10 threw:thrown | a0,closed at 1,closed at 2");
    shouldBe(run("none", "nnr"), "00 01 done:102 | closed at 2,closed at 1");
    shouldBe(run("none", "nnnnt"), "00 01 02 10 threw:104 | a0,closed at 1,closed at 2");

    function* overArray(array) {
        let sum = 0;
        for (let x of array) {
            sum += x;
            yield sum;
        }
        return sum;
    }
    shouldBe(drive(overArray([1, 2, 3]), "nnnn"), "1 3 6 done:6");
    shouldBe(drive(overArray([1, 2, 3]), "nrn"), "1 done:101 done:undefined");
});

test("locals in their temporal dead zone", () => {
    function* g() {
        yield 1;
        try {
            opaque(x);
        } catch (error) {
            yield error instanceof ReferenceError;
        }
        yield 2;
        try {
            x = 3;
        } catch (error) {
            yield error instanceof ReferenceError;
        }
        let x = 5;
        yield x;
        x++;
        yield x;
    }
    shouldBe(drive(g(), "nnnnnnn"), "1 true 2 true 5 6 done:undefined");

    function* inLoop() {
        for (let i = 0; i < 2; i++) {
            yield i;
            try {
                opaque(x);
            } catch (error) {
                yield error instanceof ReferenceError;
            }
            let x = i;
            yield x;
        }
    }
    shouldBe(drive(inLoop(), "nnnnnnn"), "0 true 0 1 true 1 done:undefined");
});

test("vars nobody assigned", () => {
    function* g(assign) {
        yield 1;
        if (assign)
            var a = 5;
        yield 2;
        return a;
    }
    shouldBe(drive(g(false), "nnn"), "1 2 done:undefined");
    shouldBe(drive(g(true), "nnn"), "1 2 done:5");
});

let outer = 40;
test("the scope register", () => {
    function* unchanged() {
        yield outer;
        yield outer + 1;
        return outer + 2;
    }
    shouldBe(drive(unchanged(), "nnn"), "40 41 done:42");

    function* changedLater() {
        yield outer;
        yield outer + 1;
        let closure;
        {
            let captured = 2;
            closure = () => captured + outer;
            yield closure();
        }
        yield outer + 3;
        return closure() + 2;
    }
    shouldBe(drive(changedLater(), "nnnnn"), "40 41 42 43 done:44");

    function* capturedFromTheStart() {
        let captured = 1;
        let closure = () => captured++ + outer;
        yield closure();
        yield closure();
        return captured;
    }
    shouldBe(drive(capturedFromTheStart(), "nnn"), "41 42 done:3");

    // The environment for the vars doubles as the generator frame, so it and the scope register are written before there is a frame.
    function* capturedVar() {
        var captured = 1;
        var closure = () => captured++ + outer;
        yield closure();
        yield closure() + outer;
        return captured;
    }
    shouldBe(drive(capturedVar(), "nnn"), "41 82 done:3");

    function* blockScopes() {
        let closures = [];
        for (let i = 0; i < 3; i++) {
            let captured = i * 10;
            closures.push(() => captured + outer);
            yield closures.length;
            captured++;
        }
        yield outer;
        return closures.map(closure => closure()).join();
    }
    shouldBe(drive(blockScopes(), "nnnnn"), "1 2 3 40 done:41,51,61");

    function* catchScopes(fail) {
        let captured = 1;
        let closure = () => captured;
        try {
            {
                let inner = 2;
                closure = () => captured + inner;
                yield closure();
                throwIf(fail);
                yield closure() + 1;
            }
        } catch (error) {
            yield closure() + outer;
            captured = 5;
        }
        yield closure() + outer;
        return outer;
    }
    shouldBe(drive(catchScopes(true), "nnnn"), "3 43 47 done:40");
    shouldBe(drive(catchScopes(false), "nnnn"), "3 4 43 done:40");
    shouldBe(drive(catchScopes(false), "ntnn"), "3 43 47 done:40");

    let sloppy = new Function("object", "return (function* () { with (object) { yield x; yield x + 1; } yield typeof x; })();");
    shouldBe(drive(sloppy({ x: 7 }), "nnn"), "7 8 undefined");
});

test("this, new.target, arguments", () => {
    function* g(a, b) {
        yield this.name;
        yield arguments.length;
        a = 9;
        yield arguments[0] + arguments[1];
        yield (() => this.name + arguments[1])();
        return this.name;
    }
    shouldBe(drive(g.call({ name: "self" }, 1, 2), "nnnnn"), "self 2 11 self2 done:self");

    class Base {
        constructor() { this.made = new.target.name; }
        async method(value) {
            let before = this.made;
            await value;
            return before + this.made + (await value);
        }
    }
    class Derived extends Base {
        async method(value) {
            await value;
            return "d" + await super.method(value);
        }
    }
    shouldBe(settle(new Derived().method(1)), "value:dDerivedDerived1");
});

test("temporaries", () => {
    function join(...values) { return values.join(); }
    function* g(a, b) {
        let sum = a + (yield 1) + b;
        let called = join(a, yield 2, b, yield 3);
        let array = [a, yield 4, ...[b, yield 5]];
        let object = { [a]: yield 6, b, c: yield 7 };
        let string = `${a}${yield 8}${b}`;
        let [x = yield 9, y = yield 10] = [undefined, undefined];
        let conditional = (yield 11) ? a : b;
        a += yield 12;
        return [sum, called, array, JSON.stringify(object), string, x, y, conditional, a].join("|");
    }
    shouldBe(drive(g(1, 2), "nnnnnnnnnnnnn"), "1 2 3 4 5 6 7 8 9 10 11 12 done:104|1,102,2,103|1,104,2,105|{\"1\":106,\"b\":2,\"c\":107}|11082|109|110|1|113");
});

test("delegating", () => {
    function* inner(log) {
        try {
            let a = yield "i1";
            let b = yield "i2";
            return a + b;
        } finally {
            log.push("inner done");
        }
    }
    function* g(log) {
        let before = opaque("before");
        let result;
        try {
            result = yield* inner(log);
        } catch (error) {
            result = "caught " + error;
        } finally {
            log.push(before);
        }
        yield result;
        return before;
    }
    function run(steps) {
        let log = [];
        return drive(g(log), steps) + " | " + log.join();
    }
    shouldBe(run("nnnn"), "i1 i2 203 done:before | inner done,before");
    shouldBe(run("ntn"), "i1 caught 101 done:before | inner done,before");
    shouldBe(run("nr"), "i1 done:101 | inner done,before");
});

// A load can end up right in front of a switch, whose jump table has a zero for each value between the cases that has no case.
test("a switch with gaps between its cases", () => {
    function* integers(k) {
        let v = opaque(k) | 0;
        yield 1;
        if (k > 50)
            yield 2;
        switch (v) {
        case 0:
            return "zero";
        case 1:
            return "one";
        case 3:
            return "three";
        case 6:
            return "six";
        default:
            return "default";
        }
    }
    let results = [];
    for (let k of [-1, 0, 1, 2, 3, 4, 5, 6, 7])
        results.push(drive(integers(k), "nn"));
    shouldBe(results.join(), "1 done:default,1 done:zero,1 done:one,1 done:default,1 done:three,1 done:default,1 done:default,1 done:six,1 done:default");

    function* characters(c) {
        let v = opaque(c);
        yield 1;
        if (c === "never")
            yield 2;
        switch (v) {
        case "a":
            return "A";
        case "b":
            return "B";
        case "d":
            return "D";
        }
        return "none";
    }
    results = [];
    for (let c of ["a", "b", "c", "d", "e"])
        results.push(drive(characters(c), "nn"));
    shouldBe(results.join(), "1 done:A,1 done:B,1 done:none,1 done:D,1 done:none");

    function* inLoop(values) {
        let log = [];
        for (let v of values) {
            yield v;
            switch (v) {
            case 10:
                log.push("ten");
                break;
            case 11:
                log.push("eleven");
                continue;
            case 14:
                log.push("fourteen");
                break;
            }
            log.push(v);
        }
        return log.join();
    }
    shouldBe(drive(inLoop([10, 12, 11, 13, 14]), "nnnnnn"), "10 12 11 13 14 done:ten,10,12,eleven,13,fourteen,14");
});

test("async functions", () => {
    async function f(fail) {
        let a = opaque(1);
        let b = opaque(2);
        let log = [];
        try {
            log.push(a + await 10);
            for (let i = 0; i < 3; i++) {
                b += await i;
                if (fail === i)
                    await Promise.reject("rejected");
            }
            log.push(b);
        } catch (error) {
            log.push(error, a, b);
            await null;
            log.push(a + b);
        } finally {
            log.push("finally", a);
            await null;
            log.push(b);
        }
        return log.join();
    }
    shouldBe(settle(f(-1)), "value:11,5,finally,1,5");
    shouldBe(settle(f(1)), "value:11,rejected,1,3,4,finally,1,3");
});

test("async generators and for-await", () => {
    let log = [];
    function source(count, failAt) {
        let i = 0;
        return {
            [Symbol.asyncIterator]() { return this; },
            async next() {
                throwIf(i === failAt, "source failed");
                return { done: i >= count, value: i++ };
            },
            async return() { log.push("closed at " + i); return { }; },
        };
    }
    async function* g(count, failAt) {
        let total = opaque(0);
        let label = opaque("total");
        try {
            for await (let x of source(count, failAt)) {
                total += x;
                yield total;
                total += await 100;
            }
        } finally {
            yield `${label} ${total}`;
        }
        return label;
    }
    async function consume(count, failAt, stopAt) {
        let seen = [];
        try {
            for await (let value of g(count, failAt)) {
                seen.push(value);
                if (seen.length === stopAt)
                    break;
            }
        } catch (error) {
            seen.push(error);
        }
        return seen.join();
    }
    shouldBe(settle(consume(3, -1, -1)), "value:0,101,203,total 303");
    shouldBe(settle(consume(3, 2, -1)), "value:0,101,total 201,source failed");
    shouldBe(settle(consume(3, -1, 2)), "value:0,101");
    shouldBe(log.join(), "closed at 2");
});

// Big enough for keeping locals in the frame to pay off. The same body, with the suspends taken out, says what to expect.
let manySuspendPoints = (() => {
    function source(suspend, header) {
        let locals = 12;
        let lines = [];
        for (let i = 0; i < locals; i++)
            lines.push(`let v${i} = base + ${i};`);
        lines.push("let log = [];", "for (let round = 0; round < 2; round++) {", "try {", "try {");
        for (let i = 0; i < 36; i++) {
            let a = i % locals, b = (i * 7 + 3) % locals, c = (i * 5 + 1) % locals;
            lines.push(`v${a} = (v${b} + ${suspend}) | 0;`);
            if (i % 9 === 4)
                lines.push(`for (let k = 0; k < 3; k++) v${c} = (v${c} + v${a} + k) | 0;`);
            if (i % 13 === 6)
                lines.push(`if (failAt === ${i} && !round) throw v${c};`);
            if (i % 17 === 8)
                lines.push(`try { if (failAt === ${i}) throw v${b}; v${c} ^= ${suspend}; } catch (e) { log.push(e, v${a}); } finally { v${b} = (v${b} + ${suspend}) | 0; }`);
        }
        lines.push("} catch (e) {", `log.push("caught", e, v1, v5, ${suspend}, v9);`, "} finally {", `log.push(v2, ${suspend}, v3);`, "}");
        lines.push("} finally {", "log.push(v10, v11);", "}", "}");
        let all = [];
        for (let i = 0; i < locals; i++)
            all.push(`v${i}`);
        lines.push(`return log.join() + ":" + [${all.join()}].join();`);
        return `(${header}(base, failAt) {\n${lines.join("\n")}\n})`;
    }
    return {
        plain: (0, eval)(source("(1)", "function")),
        generator: (0, eval)(source("(yield)", "function*")),
        asyncFunction: (0, eval)(source("(await 1)", "async function")),
    };
})();

test("many suspend points and many locals", () => {
    let { plain, generator, asyncFunction } = manySuspendPoints;
    for (let failAt of [-1, 6, 8, 19, 25, 32]) {
        let expected = plain(3, failAt);
        let iterator = generator(3, failAt);
        let result;
        do
            result = iterator.next(1);
        while (!result.done);
        shouldBe(result.value, expected);
        shouldBe(settle(asyncFunction(3, failAt)), "value:" + expected);
    }
});

function runGeneratorFrameTests(iterations)
{
    for (let i = 0; i < iterations; i++) {
        for (let { name, body } of cases) {
            try {
                body();
            } catch (error) {
                throw new Error(`${name}: ${error.message ?? error}`);
            }
        }
    }
}
