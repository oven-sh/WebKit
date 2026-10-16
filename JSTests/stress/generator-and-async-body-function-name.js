//@ requireOptions("--useAsyncStackTrace=1")

// The code of a generator, of an async generator and of an async function that awaits runs in a body function. The
// frame of that code takes its name from the body function. The body function takes the name of the function: the
// declared one, or the one inferred from the binding the function is assigned to.

let sink = 0; // `sink++` after a call keeps that call out of tail position.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

// The names of the functions in a stack, down to main().
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

const arrow = async () => {
    await null;
    const stack = new Error().stack;
    sink++;
    return stack;
};
const arrowBeforeAwait = async () => {
    const stack = new Error().stack;
    sink++;
    await null;
    return stack;
};
const expression = async function () {
    await null;
    const stack = new Error().stack;
    sink++;
    return stack;
};
const object = {
    property: async () => {
        await null;
        const stack = new Error().stack;
        sink++;
        return stack;
    },
};
class Class {
    static field = async () => {
        await null;
        const stack = new Error().stack;
        sink++;
        return stack;
    };
}
let assigned;
assigned = async () => {
    await null;
    const stack = new Error().stack;
    sink++;
    return stack;
};
function makeNested() {
    const nested = async () => {
        await null;
        const stack = new Error().stack;
        sink++;
        return stack;
    };
    sink++;
    return nested;
}
const generator = function* () {
    yield new Error().stack;
};
const asyncGenerator = async function* () {
    yield new Error().stack;
};
// `awaits` is not running when the stack is made: it awaits `declaration`.
const awaits = async () => {
    const stack = await declaration();
    sink++;
    return stack;
};
async function declaration() {
    await null;
    const stack = new Error().stack;
    sink++;
    return stack;
}
// No name can be inferred for these.
const inArray = [async () => {
    await null;
    const stack = new Error().stack;
    sink++;
    return stack;
}];
const key = "comp" + "uted";
const computed = {
    [key]: async () => {
        await null;
        const stack = new Error().stack;
        sink++;
        return stack;
    },
};

async function main() {
    shouldBe(frames(await arrow()), "arrow", "arrow");
    shouldBe(frames(await arrowBeforeAwait()), "arrowBeforeAwait", "arrow before its first await");
    shouldBe(frames(await expression()), "expression", "function expression");
    shouldBe(frames(await object.property()), "property", "property");
    shouldBe(frames(await Class.field()), "field", "class field");
    shouldBe(frames(await assigned()), "assigned", "assignment");
    shouldBe(frames(await makeNested()()), "nested", "nested");
    shouldBe(frames(generator().next().value), "generator", "generator");
    shouldBe(frames((await asyncGenerator().next()).value), "asyncGenerator,next", "async generator");
    shouldBe(frames(await awaits()), "declaration,async awaits", "awaiting arrow");
    shouldBe(frames(await declaration()), "declaration", "declaration");
    shouldBe(frames(await inArray[0]()), "", "array element");
    shouldBe(frames(await computed[key]()), "", "computed key");
    sink++;
}

for (let i = 0; i < testLoopCount; ++i) {
    let error;
    main().then(() => { sink++; }, rejection => { error = rejection; });
    drainMicrotasks();
    if (error)
        throw error;
}
