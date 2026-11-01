// Error.captureStackTrace() defines `stack` on its target. It refuses a target that is not extensible, and one whose own `stack` is
// not configurable, before it captures a frame. V8 does the same, with the same two messages.
function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${String(expected)} but got ${String(actual)}`);
}

function shouldThrowTypeError(operation, message) {
    let error;
    try {
        operation();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof TypeError))
        throw new Error(`expected a TypeError but got ${String(error)}: ${operation}`);
    shouldBe(error.message, message);
}

const notExtensible = "Cannot define property stack, object is not extensible";
const notConfigurable = "Cannot redefine property: stack";

function createdHere(message) {
    return new Error(message);
}

function capturedHere(target) {
    Error.captureStackTrace(target);
}

function readError() {
    const error = createdHere("read");
    error.stack;
    return error;
}

const targets = [
    () => ({ a: 1 }),
    () => ({ stack: "mine" }),
    () => [1, 2],
    () => function () { },
    () => createdHere("unread"),
    readError,
];

// Not extensible: refused whether or not the target has a `stack`, and nothing is added to it.
for (const lock of [Object.preventExtensions, Object.seal, Object.freeze]) {
    for (const create of targets) {
        const target = lock(create());
        // Listing the keys of an error reads its stack, so the keys to expect come from a twin.
        const keys = Reflect.ownKeys(lock(create())).join();
        shouldThrowTypeError(() => capturedHere(target), notExtensible);
        shouldBe(Reflect.ownKeys(target).join(), keys);
        shouldBe(Object.isExtensible(target), false);
        shouldBe(Object.isFrozen(target), lock === Object.freeze);
        if (target instanceof Error) {
            // The error keeps the stack of the place that made it.
            shouldBe(target.stack.includes("createdHere"), true);
            shouldBe(target.stack.includes("capturedHere"), false);
        } else if (Object.hasOwn(target, "stack"))
            shouldBe(target.stack, "mine");
    }
}

// Extensible, with a `stack` that is not configurable: refused, and the property stays as it is.
const lockedStacks = [
    { value: "mine", writable: false, enumerable: false, configurable: false },
    { value: "mine", writable: true, enumerable: true, configurable: false },
    { get() { return "mine"; }, set: undefined, enumerable: false, configurable: false },
];
for (const descriptor of lockedStacks) {
    for (const create of targets) {
        const target = Object.defineProperty(create(), "stack", descriptor);
        shouldThrowTypeError(() => capturedHere(target), notConfigurable);
        const now = Object.getOwnPropertyDescriptor(target, "stack");
        for (const key of Reflect.ownKeys(descriptor))
            shouldBe(now[key], descriptor[key]);
        shouldBe(target.stack, "mine");
    }
}

// A configurable `stack` is replaced, also one that is read-only.
for (const create of targets) {
    const target = Object.defineProperty(create(), "stack", { value: "mine", writable: false, enumerable: true, configurable: true });
    capturedHere(target);
    shouldBe(target.stack.includes("capturedHere"), true);
    const now = Object.getOwnPropertyDescriptor(target, "stack");
    shouldBe(now.writable, true);
    shouldBe(now.enumerable, false);
    shouldBe(now.configurable, true);
}

// A target that a first capture made it through is refused once it is locked, and keeps the first stack.
for (const create of targets) {
    const target = create();
    (function firstCapture() { Error.captureStackTrace(target); })();
    Object.preventExtensions(target);
    shouldThrowTypeError(() => capturedHere(target), notExtensible);
    shouldBe(target.stack.includes("firstCapture"), true);
    shouldBe(target.stack.includes("capturedHere"), false);
}

// A Proxy is refused like a value that is no object, and none of its traps runs.
{
    let traps = 0;
    const handler = new Proxy({}, { get() { traps++; } });
    for (const proxied of [{}, Object.freeze({}), createdHere("proxied"), function () { }]) {
        const proxy = new Proxy(proxied, handler);
        shouldThrowTypeError(() => capturedHere(proxy), "captureStackTrace expects the first argument to be an object");
        shouldBe(traps, 0);
        shouldBe(Object.hasOwn(proxied, "stack") && !(proxied instanceof Error), false);
    }
}

// What is neither locked nor a Proxy gets its `stack` as before.
for (const create of targets) {
    const target = create();
    capturedHere(target);
    shouldBe(target.stack.includes("capturedHere"), true);
    shouldBe(Object.getOwnPropertyDescriptor(target, "stack").enumerable, false);
    shouldBe(Object.isExtensible(target), true);
}

// A WebAssembly GC object takes no property at all. The store to one used to end the process.
if (this.WebAssembly) {
    // (type $s (struct (field (mut i32))))
    // (func (export "mk") (result (ref null $s)) struct.new_default $s)
    const bytes = new Uint8Array([
        0, 0x61, 0x73, 0x6d, 1, 0, 0, 0,
        1, 10, 2, 0x5f, 1, 0x7f, 1, 0x60, 0, 1, 0x63, 0,
        3, 2, 1, 1,
        7, 6, 1, 2, 0x6d, 0x6b, 0, 0,
        10, 7, 1, 5, 0, 0xfb, 1, 0, 0x0b,
    ]);
    const struct = new WebAssembly.Instance(new WebAssembly.Module(bytes)).exports.mk();
    shouldThrowTypeError(() => capturedHere(struct), notExtensible);
}

// globalThis answers isExtensible() itself: it is asked, and it is extensible until a script says otherwise.
capturedHere(globalThis);
Object.preventExtensions(globalThis);
shouldThrowTypeError(() => capturedHere(globalThis), notExtensible);
