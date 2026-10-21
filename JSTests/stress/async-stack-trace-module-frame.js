//@ requireOptions("--useAsyncStackTrace=1", "-m")

// A module that waits at a top-level await is the end of an await chain. Its frame is the last async frame.

const source = "async-stack-trace-module-frame.js";

// An async frame has no position when a collection took the CodeBlock of its code, which some test modes force.
function shouldBe(actual, expected) {
    const actualFrames = actual.split(" | ");
    const expectedFrames = expected.split(" | ");
    const matches = actualFrames.length === expectedFrames.length && expectedFrames.every((frame, i) =>
        actualFrames[i] === frame || (frame.startsWith("async ") && actualFrames[i] === frame.replace(/:\d+:\d+$/, "")));
    if (!matches)
        throw new Error(`Expected ${expected} but got ${actual}`);
}

// The URL of a module is absolute. Keep the file name.
function frames(error) {
    return error.stack.split("\n").map(line => line.replace(/@.*\//, "@")).join(" | ");
}

async function failing() {
    for (let i = 0; i < 10; i++)
        await 0;
    throw new Error("failing");
}

function thrower() {
    throw new Error("thrower");
}

async function caller() {
    await failing();
}

function thenable() {
    return { then() { thrower(); } };
}

let error;

try {
    await caller();
} catch (e) {
    error = e;
}
shouldBe(frames(error), `failing@${source}:25:20 | async caller@${source}:33:18 | async module code@${source}:43:17`);

// The module awaits a promise that then() returned.
try {
    await failing().then(value => value);
} catch (e) {
    error = e;
}
shouldBe(frames(error), `failing@${source}:25:20 | async module code@${source}:51:25`);

// The Error is made in a handler, and no async function is between the handler and the module.
try {
    await Promise.resolve(null).then(thrower);
} catch (e) {
    error = e;
}
shouldBe(frames(error), `thrower@${source}:29:20 | async module code@${source}:59:37`);

try {
    await Promise.all([caller()]);
} catch (e) {
    error = e;
}
shouldBe(frames(error), `failing@${source}:25:20 | async caller@${source}:33:18 | async module code@${source}:66:22`);

// A thenable whose then() throws: the job that calls then() settles what the module awaits.
try {
    await thenable();
} catch (e) {
    error = e;
}
shouldBe(frames(error), `thrower@${source}:29:20 | then@${source}:37:30 | async module code@${source}:74:19`);

// The module waits in a `for await`: for a step of an async generator, and for a promise of an array.
async function* generator() {
    yield await failing();
}
try {
    for await (const value of generator()) { }
} catch (e) {
    error = e;
}
shouldBe(frames(error), `failing@${source}:25:20 | async module code@${source}:85:28`);

try {
    for await (const value of [failing()]) { }
} catch (e) {
    error = e;
}
shouldBe(frames(error), `failing@${source}:25:20 | async module code@${source}:92:28`);

// No async frame for a module that does not wait: this one runs when the Error is made.
error = new Error("sync");
shouldBe(frames(error), `module code@${source}:99:18`);

// A collection can take the code of a module that waits. Its frame keeps the file.
const gate = Promise.withResolvers();
const late = (async function late() {
    await gate.promise;
    return new Error("late");
})();
$vm.deleteAllCodeWhenIdle();
setTimeout(() => gate.resolve());
error = await late;
shouldBe(frames(error), `late@${source}:106:21 | async module code@${source}`);
