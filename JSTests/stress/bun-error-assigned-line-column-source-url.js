// An Error has a line, a column and a sourceURL that the engine defines when one of them, or the stack, is first looked
// up. They are not enumerable. One that user code stores is an ordinary property of user code: writable, enumerable and
// configurable, as in V8, which defines none of its own. Upstream keeps the engine's attributes and replaces the value.
// (Section A12 of JSTests/BUN-TEST-DIFFERENCES.md.)

"use strict";

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

function shouldThrowTypeError(run, message) {
    let thrown = null;
    try {
        run();
    } catch (error) {
        thrown = error;
    }
    if (!(thrown instanceof TypeError))
        throw new Error(`${message}: expected a TypeError but got ${thrown}`);
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
const keys = error => Object.keys(error).join();

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

// A store in sloppy code, which says nothing when it is refused.
const storeSloppy = new Function("error", "name", "value", "error[name] = value;");

// The Structure that an object has now.
function structureID(object) {
    const transitions = $vm.getStructureTransitionList(object);
    return transitions[transitions.length - 5];
}

// Nobody wrote to the error: the engine's own.
{
    const error = new Error("message");
    shouldBe(describeType(error, "line"), "number W-C", "untouched line");
    shouldBe(describeType(error, "column"), "number W-C", "untouched column");
    shouldBe(keys(error), "", "untouched keys");
}

for (const name of names) {
    // A store, before anything looked at the error and after.
    for (const readsStackFirst of [false, true]) {
        const error = new RangeError("message");
        if (readsStackFirst)
            error.stack;
        error[name] = 5;
        shouldBe(describe(error, name), "5 WEC", `${name} stored, stack read first: ${readsStackFirst}`);
        shouldBe(keys(error), name, `keys after ${name} stored, stack read first: ${readsStackFirst}`);
        // The names that were not stored stay the engine's.
        for (const other of ["line", "column"]) {
            if (other !== name)
                shouldBe(describeType(error, other), "number W-C", `${other} beside a stored ${name}`);
        }
        shouldBe(typeof error.stack, "string", `stack beside a stored ${name}`);
        // The next store replaces the value of what is now an ordinary property.
        error[name] = 6;
        shouldBe(describe(error, name), "6 WEC", `${name} stored twice`);
    }

    // Other ways to the same store.
    {
        const error = new Error("message");
        shouldBe(Reflect.set(error, name, 5), true, `Reflect.set of ${name}`);
        shouldBe(describe(error, name), "5 WEC", `${name} after Reflect.set`);
        const assigned = Object.assign(new Error("message"), { [name]: 5 });
        shouldBe(describe(assigned, name), "5 WEC", `${name} after Object.assign`);
    }

    // An error that the engine threw, and one that the parser made.
    for (const source of ["null.property", "eval('{')"]) {
        let error;
        try {
            (0, eval)(source);
        } catch (thrown) {
            error = thrown;
        }
        error[name] = 5;
        shouldBe(describe(error, name), "5 WEC", `${name} stored on the error of ${source}`);
        shouldBe(keys(error), name, `keys of the error of ${source}`);
    }

    // A property that user code defined by hand with the engine's attributes keeps them.
    {
        const error = new Error("message");
        Object.defineProperty(error, name, { value: 5, writable: true, enumerable: false, configurable: true });
        error[name] = 6;
        shouldBe(describe(error, name), "6 W-C", `${name} defined by hand, then stored`);
    }

    // A definition is applied to what the engine defined, as it was before.
    {
        const error = new Error("message");
        Object.defineProperty(error, name, { value: 5 });
        shouldBe(describe(error, name), "5 W-C", `${name} defined with a value only`);
        error[name] = 6;
        shouldBe(describe(error, name), "6 W-C", `${name} defined with a value only, then stored`);
    }

    // A complete definition gives the property what it says, and a class field is one.
    for (const readsStackFirst of [false, true]) {
        const error = new Error("message");
        if (readsStackFirst)
            error.stack;
        Object.defineProperty(error, name, { value: 5, writable: true, enumerable: true, configurable: true });
        error.stack;
        shouldBe(describe(error, name), "5 WEC", `${name} defined completely, stack read first: ${readsStackFirst}`);
        shouldBe(keys(error), name, `keys after ${name} defined completely`);

        class WithField extends Error {
            [name] = 5;
        }
        const withField = new WithField("message");
        shouldBe(describe(withField, name), "5 WEC", `${name} as a class field`);
        shouldBe(keys(withField), name, `keys of an error with the field ${name}`);
    }

    // A deleted name stays deleted, and a store makes an ordinary property.
    for (const readsStackFirst of [false, true]) {
        const error = new Error("message");
        if (readsStackFirst)
            error.stack;
        shouldBe(delete error[name], true, `delete ${name}`);
        shouldBe(name in error, false, `${name} after delete`);
        shouldBe(typeof error.stack, "string", `stack after delete ${name}`);
        shouldBe(describe(error, name), "none", `${name} after delete and a stack read`);
        error[name] = 5;
        shouldBe(describe(error, name), "5 WEC", `${name} deleted, then stored`);
    }

    // A frozen or sealed error keeps what the engine defined.
    {
        const frozen = Object.freeze(new Error("message"));
        const before = describe(frozen, name);
        shouldThrowTypeError(() => { frozen[name] = 5; }, `${name} stored on a frozen error`);
        shouldBe(describe(frozen, name), before, `${name} of a frozen error`);

        const sealed = Object.seal(new Error("message"));
        if (name in sealed) {
            sealed[name] = 5;
            shouldBe(describe(sealed, name), "5 W--", `${name} stored on a sealed error`);
        }
    }

    // An error that takes no more properties: one that the engine did not define yet cannot be added, as in V8. One that
    // it defined takes the value.
    {
        const error = Object.preventExtensions(new Error("message"));
        shouldThrowTypeError(() => { error[name] = 5; }, `${name} stored on an error that is not extensible`);
        shouldBe(describe(error, name), "none", `${name} of an error that is not extensible`);

        const sloppy = Object.preventExtensions(new Error("message"));
        storeSloppy(sloppy, name, 5);
        shouldBe(describe(sloppy, name), "none", `${name} of an error that is not extensible, after a sloppy store`);

        const read = new Error("message");
        read.stack;
        Object.preventExtensions(read);
        if (name in read) {
            read[name] = 5;
            shouldBe(describe(read, name), "5 W-C", `${name} stored on an error that is not extensible, stack read first`);
        }
    }

    // An error whose properties are immutable keeps what it has from the start.
    {
        const error = $vm.makePropertiesImmutable(new Error("message"));
        shouldThrowTypeError(() => { error[name] = 5; }, `${name} stored on an error with immutable properties`);
        shouldBe(Reflect.deleteProperty(error, name), !(name in error), `${name} deleted from an error with immutable properties`);
        if (name !== "sourceURL")
            shouldBe(describeType(error, name), "number W-C", `${name} of an error with immutable properties`);
    }

    // An accessor or a read-only property of the same name on the prototype of a subclass answers the store.
    for (const readsStackFirst of [false, true]) {
        class WithSetter extends Error {
            set [name](value) {
                this.stored = value;
            }
        }
        const error = new WithSetter("message");
        if (readsStackFirst)
            error.stack;
        error[name] = 5;
        shouldBe(error.stored, 5, `the setter of ${name} on the prototype, stack read first: ${readsStackFirst}`);
        shouldBe(describe(error, name), "none", `${name} behind a setter on the prototype`);

        class ReadOnly extends Error { }
        Object.defineProperty(ReadOnly.prototype, name, { value: 1, writable: false });
        const refused = new ReadOnly("message");
        if (readsStackFirst)
            refused.stack;
        shouldThrowTypeError(() => { refused[name] = 5; }, `${name} stored below a read-only ${name}`);
        shouldBe(refused[name], 1, `${name} below a read-only ${name}`);

        const refusedSloppy = new ReadOnly("message");
        storeSloppy(refusedSloppy, name, 5);
        shouldBe(refusedSloppy[name], 1, `${name} below a read-only ${name}, after a sloppy store`);

        // Until the first store, a read finds the engine's own property in front of what the class declares.
        class WithGetter extends Error {
            get [name]() {
                return "of the class";
            }
        }
        const declared = new WithGetter("message");
        if (readsStackFirst)
            declared.stack;
        if (name in new Error("message"))
            shouldBe(declared[name] === "of the class", false, `${name} that the class declares, before a store`);
        shouldThrowTypeError(() => { declared[name] = 5; }, `${name} stored below a getter without a setter`);
        shouldBe(declared[name], "of the class", `${name} that the class declares, after a store`);
    }

    // A store that an object below the error receives only passes through: the error keeps the engine's.
    {
        const error = new Error("message");
        const heir = Object.create(error);
        heir[name] = 5;
        shouldBe(describe(heir, name), "5 WEC", `${name} stored on an heir`);
        shouldBe(keys(error), "", `keys of the error above the heir`);
        if (name !== "sourceURL")
            shouldBe(describeType(error, name), "number W-C", `${name} of the error above the heir`);
    }
}

// The keys come in the order of the stores, whatever the engine defined first.
{
    const error = new Error("message");
    error.stack;
    error.sourceURL = "source";
    error.column = 2;
    error.line = 1;
    shouldBe(keys(error), "sourceURL,column,line", "keys in the order of the stores");
    shouldBe(JSON.stringify(error), '{"sourceURL":"source","column":2,"line":1}', "JSON.stringify");
    const seen = [];
    for (const key in error)
        seen.push(key);
    shouldBe(seen.join(), "sourceURL,column,line", "for-in");
}

// The shape of postcss's CssSyntaxError.
{
    class CssSyntaxError extends Error {
        constructor(message, line, column) {
            super(message);
            this.name = "CssSyntaxError";
            this.line = line;
            this.column = column;
        }
    }
    const error = new CssSyntaxError("Unclosed block", 1, 2);
    shouldBe(keys(error), "name,line,column", "keys of a CssSyntaxError");
    shouldBe(describe(error, "line"), "1 WEC", "line of a CssSyntaxError");
    shouldBe(describe(error, "column"), "2 WEC", "column of a CssSyntaxError");
}

// Every own name of one error stored on another: the engine's names are among them, and each store makes an ordinary
// property of the copy. V8 has no such names to copy.
{
    const source = new Error("message");
    const copy = new Error("message");
    const ownNames = Object.getOwnPropertyNames(source);
    for (const name of ownNames)
        copy[name] = source[name];
    shouldBe(keys(copy), ownNames.filter(name => names.includes(name)).join(), "keys of a copy of every own name");
    shouldBe(copy.line, source.line, "line of a copy of every own name");
}

// Errors that are written the same way have one Structure: the store that takes a name over leaves no Structure of its own.
{
    const makers = {
        "stored": () => {
            const error = new Error("message");
            error.line = 1;
            error.column = 2;
            error.sourceURL = "source";
            return error;
        },
        "stack read, then stored": () => {
            const error = new Error("message");
            error.stack;
            error.line = 1;
            error.column = 2;
            error.sourceURL = "source";
            return error;
        },
        "stored in a constructor": () => {
            class CssSyntaxError extends Error {
                constructor(message, line, column) {
                    super(message);
                    this.name = "CssSyntaxError";
                    this.line = line;
                    this.column = column;
                }
            }
            return new CssSyntaxError("Unclosed block", 1, 2);
        },
    };
    for (const [how, make] of Object.entries(makers)) {
        make();
        const first = make();
        const second = make();
        if (how !== "stored in a constructor")
            shouldBe(structureID(first), structureID(second), `the Structure of two errors, ${how}`);
        shouldBe(keys(first), keys(second), `the keys of two errors, ${how}`);
    }
}

// One store site over errors whose line user code defined by hand, with the engine's attributes, and errors that nobody
// wrote to. Both have the same Structure. The first keep the attributes, the second get an ordinary property.
function storeLineAtSharedSite(error, value) {
    error.line = value;
}
noInline(storeLineAtSharedSite);

for (let i = 0; i < testLoopCount; i++) {
    const byHand = (i % 8) !== 7;
    const error = new Error("message");
    error.stack;
    if (byHand)
        Object.defineProperty(error, "line", { value: 5 });
    storeLineAtSharedSite(error, i);
    if (describe(error, "line") !== (byHand ? `${i} W-C` : `${i} WEC`))
        throw new Error(`line stored at a shared site, iteration ${i}, defined by hand: ${byHand}: ${describe(error, "line")}`);
}

// What a TypeScript class with declared fields compiles to: the fields are defined, then the constructor stores.
class WithFields extends Error {
    column = 0;
    line;
    constructor(message, line) {
        super(message);
        this.line = line;
    }
}

// One store site, over fresh errors, some of them looked at before: every tier gives the same property.
for (let i = 0; i < testLoopCount; i++) {
    const error = new Error("message");
    if (i & 1)
        error.stack;
    const name = names[i % 3];
    store(error, name, i);
    if (describe(error, name) !== `${i} WEC`)
        throw new Error(`${name} stored at a hot site, iteration ${i}: ${describe(error, name)}`);
    if (keys(error) !== name)
        throw new Error(`keys after a store at a hot site, iteration ${i}: ${keys(error)}`);
    store(error, name, -i);
    if (describe(error, name) !== `${-i} WEC`)
        throw new Error(`${name} stored twice at a hot site, iteration ${i}: ${describe(error, name)}`);

    const other = new Error("message");
    if (i & 2)
        other.stack;
    storeSourceURL(other, "source");
    storeColumn(other, i);
    storeLine(other, i + 1);
    if (describe(other, "line") !== `${i + 1} WEC` || describe(other, "column") !== `${i} WEC` || describe(other, "sourceURL") !== '"source" WEC')
        throw new Error(`stores by name at hot sites, iteration ${i}: ${describe(other, "line")}, ${describe(other, "column")}, ${describe(other, "sourceURL")}`);
    if (keys(other) !== "sourceURL,column,line")
        throw new Error(`keys after stores by name at hot sites, iteration ${i}: ${keys(other)}`);

    // A field is a definition, which is applied to what the engine defined: the keys are in the engine's order, not in the
    // order of the fields as in V8.
    const withFields = new WithFields("message", i);
    if (describe(withFields, "line") !== `${i} WEC` || describe(withFields, "column") !== "0 WEC" || keys(withFields) !== "line,column")
        throw new Error(`class fields in a hot constructor, iteration ${i}: ${describe(withFields, "line")}, ${describe(withFields, "column")}, ${keys(withFields)}`);
}
