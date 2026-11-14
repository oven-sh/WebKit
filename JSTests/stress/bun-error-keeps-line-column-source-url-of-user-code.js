// An Error gets a line, a column and a sourceURL from the engine when one of them, or the stack, is first looked up. What
// user code stored, defined or deleted under one of the three names stays as user code left it whenever the engine
// materializes: also when the frames arrive later (a host gives an error that was made without frames its frames with
// ErrorInstance::setStackFrames, here $vm.setStackFramesOfError), and also at a store site that an inline cache answers.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

// The value and W(ritable), E(numerable), C(onfigurable).
function describe(object, name) {
    const descriptor = Object.getOwnPropertyDescriptor(object, name);
    if (!descriptor)
        return "none";
    if (!("value" in descriptor))
        return "accessor";
    return `${JSON.stringify(descriptor.value)} ${descriptor.writable ? "W" : "-"}${descriptor.enumerable ? "E" : "-"}${descriptor.configurable ? "C" : "-"}`;
}

// The type of the value in place of the value, for what the engine defined.
function describeType(object, name) {
    const descriptor = Object.getOwnPropertyDescriptor(object, name);
    if (!descriptor)
        return "none";
    return `${typeof descriptor.value} ${descriptor.writable ? "W" : "-"}${descriptor.enumerable ? "E" : "-"}${descriptor.configurable ? "C" : "-"}`;
}

const names = ["line", "column", "sourceURL"];
// What the engine defines for an error of this file.
const engineDefines = name => name in new Error("message");

function withoutFrames(make = () => new Error("message")) {
    const limit = Error.stackTraceLimit;
    Error.stackTraceLimit = 0;
    try {
        return make();
    } finally {
        Error.stackTraceLimit = limit;
    }
}

function giveFrames(error) {
    shouldBe($vm.setStackFramesOfError(error), true, "the error takes frames");
}

function store(error, name, value) {
    error[name] = value;
}
noInline(store);

function storeLine(error, value) {
    error.line = value;
}
noInline(storeLine);

function storeColumn(error, value) {
    error.column = value;
}
noInline(storeColumn);

function storeSourceURL(error, value) {
    error.sourceURL = value;
}
noInline(storeSourceURL);

const storeByName = { line: storeLine, column: storeColumn, sourceURL: storeSourceURL };

// An error that nobody wrote to gets the position of the frames that arrive later.
{
    const error = withoutFrames();
    shouldBe(names.map(name => describe(error, name)).join(), "none,none,none", "an error made without frames");
    giveFrames(error);
    shouldBe(describeType(error, "line"), "number W-C", "line of an error that got frames");
    shouldBe(describeType(error, "column"), "number W-C", "column of an error that got frames");
    shouldBe(typeof error.stack, "string", "stack of an error that got frames");
}

for (const name of names) {
    // A store, then the frames arrive.
    {
        const error = withoutFrames();
        error[name] = 5;
        giveFrames(error);
        shouldBe(describe(error, name), "5 WEC", `${name} stored, then the frames arrive`);
        shouldBe(Object.keys(error).join(), name, `keys after ${name} stored, then the frames arrive`);
        shouldBe(typeof error.stack, "string", `stack after ${name} stored, then the frames arrive`);
        shouldBe(describe(error, name), "5 WEC", `${name} stored, the frames arrive, the stack is read`);
        // The names that nobody wrote are the engine's.
        for (const other of ["line", "column"]) {
            if (other !== name)
                shouldBe(describeType(error, other), "number W-C", `${other} beside a stored ${name}`);
        }
        // A second store materializes nothing more, and the next one may come from an inline cache.
        error[name] = 6;
        error[name] = 7;
        shouldBe(describe(error, name), "7 WEC", `${name} stored again after the frames arrived`);
    }

    // A lookup of a name that user code owns does not need the frames: it does not materialize the error. A lookup of a
    // name that is the engine's does.
    {
        const error = withoutFrames();
        error[name] = 5;
        giveFrames(error);
        shouldBe(error[name], 5, `${name} read after the frames arrive`);
        shouldBe(name in error, true, `${name} in the error after the frames arrive`);
        shouldBe(Object.prototype.hasOwnProperty.call(error, name), true, `own ${name} after the frames arrive`);
        shouldBe(describe(error, name), "5 WEC", `descriptor of ${name} after the frames arrive`);
        shouldBe($vm.setStackFramesOfError(error), true, `lookups of a stored ${name} leave the error not materialized`);
        const other = name === "line" ? "column" : "line";
        shouldBe(typeof error[other], "number", `${other} of the engine beside a stored ${name}`);
        shouldBe($vm.setStackFramesOfError(error), false, `a lookup of the engine's ${other} materializes the error`);
        shouldBe(describe(error, name), "5 WEC", `${name} after the engine's ${other} was looked up`);

        const deleted = withoutFrames();
        delete deleted[name];
        giveFrames(deleted);
        shouldBe(name in deleted, false, `deleted ${name} in the error after the frames arrive`);
        shouldBe(deleted[name], undefined, `deleted ${name} read after the frames arrive`);
        shouldBe($vm.setStackFramesOfError(deleted), true, `lookups of a deleted ${name} leave the error not materialized`);
    }

    // A store, the frames arrive, and the next thing that looks at the error is a store of the same name. That store
    // materializes, which changes the Structure under it: no inline cache may be made of it.
    {
        const error = withoutFrames();
        error[name] = 5;
        giveFrames(error);
        error[name] = 6;
        shouldBe(describe(error, name), "6 WEC", `${name} stored, the frames arrive, stored`);
        shouldBe(typeof error.stack, "string", `stack after ${name} stored, the frames arrive, stored`);

        // The same with the engine's own Error.captureStackTrace, which leaves a stack to materialize.
        const captured = withoutFrames();
        captured[name] = 5;
        Error.captureStackTrace(captured);
        captured[name] = 6;
        shouldBe(describe(captured, name), "6 WEC", `${name} stored, captured, stored`);

        const framed = new Error("message");
        framed[name] = 5;
        delete framed.stack;
        Error.captureStackTrace(framed);
        framed[name] = 6;
        shouldBe(framed[name], 6, `${name} stored, stack deleted, captured, stored`);
        shouldBe(typeof framed.stack, "string", `stack after ${name} stored, stack deleted, captured, stored`);
    }

    // A deleted name stays deleted.
    {
        const error = withoutFrames();
        shouldBe(delete error[name], true, `delete ${name} from an error made without frames`);
        giveFrames(error);
        shouldBe(typeof error.stack, "string", `stack after delete ${name}, then the frames arrive`);
        shouldBe(describe(error, name), "none", `${name} deleted, then the frames arrive`);
        shouldBe(name in error, false, `${name} in an error after delete`);
        shouldBe(Object.getOwnPropertyNames(error).includes(name), false, `${name} in the own names after delete`);

        for (const readsStackFirst of [false, true]) {
            const framed = new Error("message");
            if (readsStackFirst)
                framed.stack;
            shouldBe(delete framed[name], true, `delete ${name}, stack read first: ${readsStackFirst}`);
            shouldBe(typeof framed.stack, "string", `stack after delete ${name}`);
            shouldBe(describe(framed, name), "none", `${name} after delete and a stack read`);
        }
    }

    // What user code defined stays: an accessor is not called and not replaced, a locked property keeps its value.
    {
        let calls = 0;
        const withAccessor = withoutFrames();
        Object.defineProperty(withAccessor, name, { get() { calls++; return 9; }, configurable: true });
        const locked = withoutFrames();
        Object.defineProperty(locked, name, { value: 5, enumerable: true });
        const hidden = withoutFrames();
        Object.defineProperty(hidden, name, { value: 5, writable: true, configurable: true });
        for (const error of [withAccessor, locked, hidden]) {
            giveFrames(error);
            error.stack;
        }
        shouldBe(describe(withAccessor, name), "accessor", `${name} defined as an accessor, then the frames arrive`);
        shouldBe(calls, 0, `calls of the accessor of ${name}`);
        shouldBe(describe(locked, name), "5 -E-", `${name} defined read-only, then the frames arrive`);
        shouldBe(describe(hidden, name), "5 W-C", `${name} defined with the engine's attributes, then the frames arrive`);
    }

    // A definition is applied to what the engine defined.
    if (engineDefines(name)) {
        const error = new Error("message");
        Object.defineProperty(error, name, { value: 5 });
        shouldBe(describe(error, name), "5 W-C", `${name} defined with a value only`);
    }

    // An error whose properties are immutable has the engine's names from the start. A delete that is the first thing to
    // look at the error is refused like any other.
    if (engineDefines(name)) {
        for (const makeImmutable of [error => $vm.makePropertiesImmutable(error), error => $vm.makePropertiesImmutable(error, [name])]) {
            const error = makeImmutable(new Error("message"));
            shouldBe(Reflect.deleteProperty(error, name), false, `${name} deleted first from an error with immutable properties`);
            shouldBe(describe(error, name).slice(-3), "W-C", `${name} of an error with immutable properties after the delete`);
        }
    }

    // An error whose Structure is a dictionary.
    for (const toDictionary of [error => $vm.toCacheableDictionary(error), error => $vm.toUncacheableDictionary(error)]) {
        const untouched = toDictionary(new Error("message"));
        shouldBe(Object.keys(untouched).join(), "", "keys of an untouched dictionary error");
        if (engineDefines(name))
            shouldBe(describe(untouched, name).slice(-3), "W-C", `${name} of an untouched dictionary error`);

        const late = toDictionary(withoutFrames());
        late[name] = 5;
        giveFrames(late);
        late[name] = 6;
        shouldBe(describe(late, name), "6 WEC", `${name} of a dictionary error stored, the frames arrive, stored`);

        const deleted = toDictionary(new Error("message"));
        shouldBe(delete deleted[name], true, `delete ${name} from a dictionary error`);
        deleted.stack;
        shouldBe(describe(deleted, name), "none", `${name} deleted from a dictionary error, then a stack read`);
    }
}

// Properties that native code defined without a store are not marked, and do not have the engine's attributes: they stay.
{
    const error = new Error("message");
    shouldBe($vm.putDirectPropertyRun(error, names, [5, 6, "source"]), true, "native code defines the three names");
    error.stack;
    shouldBe(names.map(name => describe(error, name)).join(), '5 WEC,6 WEC,"source" WEC', "names that native code defined, then a stack read");
    error.line = 7;
    shouldBe(describe(error, "line"), "7 WEC", "a name that native code defined, then stored");
}

// The line that the parser gives the SyntaxError of a function body is the engine's: frames that arrive later replace it.
{
    const error = withoutFrames(() => {
        try {
            new Function("{");
        } catch (thrown) {
            return thrown;
        }
    });
    shouldBe(describeType(error, "line"), "number W-C", "line of a SyntaxError made without frames");
    const parserLine = error.line;
    error.column = 5;
    giveFrames(error);
    shouldBe(describeType(error, "line"), "number W-C", "line of a SyntaxError that got frames");
    shouldBe(error.line !== parserLine, true, "the line of a SyntaxError follows the frames");
    shouldBe(describe(error, "column"), "5 WEC", "column stored on a SyntaxError, then the frames arrive");
}

// The same sequences at store sites that run in every tier.
for (let i = 0; i < testLoopCount; i++) {
    const name = names[i % 3];

    const error = withoutFrames();
    store(error, name, i);
    giveFrames(error);
    if (i & 1)
        error.stack;
    store(error, name, -i);
    if (describe(error, name) !== `${-i} WEC` || typeof error.stack !== "string")
        throw new Error(`${name} stored by value, the frames arrive, stored, iteration ${i}: ${describe(error, name)}`);

    const other = i & 2 ? withoutFrames() : new Error("message");
    storeByName[name](other, i);
    if ((i & 5) === 5)
        delete other.stack;
    if (i & 1)
        Error.captureStackTrace(other);
    else if (i & 2)
        giveFrames(other);
    storeByName[name](other, -i);
    if (other[name] !== -i || typeof other.stack !== "string")
        throw new Error(`${name} stored by name, then frames, stored, iteration ${i}: ${describe(other, name)}`);
}

// A store that a frozen error refuses changes nothing, and says nothing in sloppy code.
for (const name of names) {
    const frozen = Object.freeze(new Error("message"));
    const before = describe(frozen, name);
    frozen[name] = 5;
    shouldBe(describe(frozen, name), before, `${name} of a frozen error after a sloppy store`);
    shouldBe(Reflect.set(frozen, name, 5), false, `Reflect.set of ${name} on a frozen error`);
}
