//@ requireOptions("--useAsyncStackTrace=1")

// Error.captureStackTrace(object, fn) starts the trace below the topmost activation of fn. The code of a generator,
// of an async generator and of an async function that awaits runs in a body function, so the frame of that body
// function is the activation, on the stack or in the await chain.

let sink = 0; // `sink++` after a call keeps that call out of tail position.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

// The names of the functions in the trace, down to main().
function frames(stack) {
    const names = [];
    for (const line of stack.split("\n")) {
        if (!line)
            continue;
        const name = line.slice(0, line.indexOf("@"));
        if (name === "main" || name === "async main")
            break;
        names.push(name);
    }
    return names.join(",");
}

function capture(hide) {
    const object = {};
    Error.captureStackTrace(object, hide);
    sink++;
    return frames(object.stack);
}

async function theCaller(fn) {
    const result = await fn();
    sink++;
    return result;
}

async function declaration() {
    await null;
    const result = capture(declaration);
    sink++;
    return result;
}
const arrow = async () => {
    await null;
    const result = capture(arrow);
    sink++;
    return result;
};
const expression = async function () {
    await null;
    const result = capture(expression);
    sink++;
    return result;
};
const key = "comp" + "uted";
const computed = {
    async [key]() {
        await null;
        const result = capture(computed[key]);
        sink++;
        return result;
    },
};
async function renamed() {
    await null;
    const result = capture(renamed);
    sink++;
    return result;
}
Object.defineProperty(renamed, "name", { value: "somethingElse" });

// Before the first await the body function runs on the frame of the function, which is hidden.
const arrowBeforeAwait = async () => {
    const result = capture(arrowBeforeAwait);
    sink++;
    await null;
    return result;
};
const arrowWithoutAwait = async () => {
    const result = capture(arrowWithoutAwait);
    sink++;
    return result;
};

// A function with the name of `fn` is above `fn` on the stack.
const sameName = {
    async work() {
        await null;
        const helper = {
            work() {
                const result = capture(sameName.work);
                sink++;
                return result;
            },
        };
        const result = helper.work();
        sink++;
        return result;
    },
};
// A function with the name of `fn` is on the stack, and `fn` is not.
function twin() { }
const otherTwin = {
    twin() {
        const result = capture(twin);
        sink++;
        return result;
    },
};
function callsOtherTwin() {
    const result = otherTwin.twin();
    sink++;
    return result;
}

const generator = function* () {
    const result = capture(generator);
    sink++;
    yield result;
};
function callerOfGenerator() {
    const result = generator().next().value;
    sink++;
    return result;
}
const generatorResumed = function* () {
    yield 1;
    const result = capture(generatorResumed);
    sink++;
    yield result;
};
function callerOfGeneratorResumed() {
    const iterator = generatorResumed();
    iterator.next();
    const result = iterator.next().value;
    sink++;
    return result;
}
const asyncGenerator = async function* () {
    const result = capture(asyncGenerator);
    sink++;
    yield result;
};
async function callerOfAsyncGenerator() {
    const result = (await asyncGenerator().next()).value;
    sink++;
    return result;
}

// `fn` is not running: it awaits the function that captures.
async function awaiting() {
    const result = await awaited();
    sink++;
    return result;
}
async function awaited() {
    await null;
    const result = capture(awaiting);
    sink++;
    return result;
}
const awaitingArrow = async () => {
    const result = await awaitedByArrow();
    sink++;
    return result;
};
async function awaitedByArrow() {
    await null;
    const result = capture(awaitingArrow);
    sink++;
    return result;
}
async function notAwaiting() { }
async function awaitedByNobody() {
    await null;
    const result = capture(notAwaiting);
    sink++;
    return result;
}

// Two closures of one function expression. Each closes over its own `tag`, so their scopes tell them apart.
const makeHandler = tag =>
    async function handler(next) {
        await null;
        const result = next ? await next() : tag;
        sink++;
        return result;
    };
const outerHandler = makeHandler("outer");
const innerHandler = makeHandler("inner");
async function callerOfHandlers() {
    const result = await outerHandler(() =>
        innerHandler(async () => {
            await null;
            const result = capture(outerHandler);
            sink++;
            return result;
        }));
    sink++;
    return result;
}
// Two closures of one function expression in one scope, before their first await. The body function of the inner one
// runs on the frame of the inner one, and that frame tells them apart.
const makePlain = () =>
    async function plain(callback) {
        const result = callback();
        sink++;
        await null;
        return result;
    };
const outerPlain = makePlain();
const innerPlain = makePlain();
function callerOfPlain(hide) {
    let result;
    outerPlain(() => {
        innerPlain(() => {
            result = capture(hide);
            sink++;
        });
        sink++;
    });
    sink++;
    return result;
}

// Not a function with activations: these do not limit the trace.
function withBound() {
    const result = capture(withBound.bind(null));
    sink++;
    return result;
}
function withProxy() {
    const result = capture(new Proxy(withProxy, {}));
    sink++;
    return result;
}
function withObject() {
    const result = capture({});
    sink++;
    return result;
}
// Functions without an activation.
function notOnStack() { }
function withNotOnStack() {
    const result = capture(notOnStack);
    sink++;
    return result;
}
function withBuiltinConstructor() {
    const result = capture(Promise);
    sink++;
    return result;
}

// Two functions that await each other make the await chain a cycle. The search for a function that is not in the
// chain has to end.
function capturesInCycleOfAwaits(hide) {
    let first;
    let second;
    let result;
    async function captures() {
        await null;
        result = capture(hide);
        sink++;
    }
    async function awaitsSecond() {
        await null;
        const value = await Promise.race([second, captures()]);
        sink++;
        return value;
    }
    async function awaitsFirst() {
        await null;
        const value = await first;
        sink++;
        return value;
    }
    first = awaitsSecond();
    second = awaitsFirst();
    drainMicrotasks();
    return result;
}

async function main() {
    shouldBe(await theCaller(declaration), "async theCaller", "declaration");
    shouldBe(await theCaller(arrow), "async theCaller", "arrow");
    shouldBe(await theCaller(expression), "async theCaller", "expression");
    shouldBe(await theCaller(computed[key]), "async theCaller", "computed key");
    shouldBe(await theCaller(renamed), "async theCaller", "renamed");
    shouldBe(await theCaller(arrowBeforeAwait), "theCaller", "arrow before its first await");
    shouldBe(await theCaller(arrowWithoutAwait), "theCaller", "arrow without await");
    shouldBe(await theCaller(sameName.work), "async theCaller", "same name above");
    shouldBe(callsOtherTwin(), "", "same name only");
    shouldBe(callerOfGenerator(), "callerOfGenerator", "generator");
    shouldBe(callerOfGeneratorResumed(), "callerOfGeneratorResumed", "generator resumed");
    shouldBe(await callerOfAsyncGenerator(), "next,callerOfAsyncGenerator", "async generator");
    shouldBe(await theCaller(awaiting), "async theCaller", "awaiting");
    shouldBe(await theCaller(awaitingArrow), "async theCaller", "awaiting arrow");
    shouldBe(await theCaller(awaitedByNobody), "", "not awaiting");
    shouldBe(await callerOfHandlers(), "async callerOfHandlers", "closure with its own scope");
    shouldBe(callerOfPlain(outerPlain), "callerOfPlain", "outer closure before its first await");
    shouldBe(callerOfPlain(innerPlain), ",plain,callerOfPlain", "inner closure before its first await");
    shouldBe(withBound(), "capture,withBound", "bound function");
    shouldBe(withProxy(), "capture,withProxy", "Proxy");
    shouldBe(withObject(), "capture,withObject", "object");
    shouldBe(withNotOnStack(), "", "function not on the stack");
    shouldBe(withBuiltinConstructor(), "", "builtin constructor");
    sink++;
}

for (let i = 0; i < testLoopCount; ++i) {
    let error;
    main().then(() => { sink++; }, rejection => { error = rejection; });
    drainMicrotasks();
    if (error)
        throw error;
}

shouldBe(capturesInCycleOfAwaits(notAwaiting), "", "function that is not in a cycle of awaits");
