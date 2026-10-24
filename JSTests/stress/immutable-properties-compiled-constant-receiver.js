// An assignment to a constant receiver is compiled and runs many times: it calls a setter, own or inherited, or stores into a data
// property. Then the receiver's properties are made immutable, and the next assignment is refused as it is in the interpreter.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + String(expected) + " but got " + String(actual));
}

let calls = 0;
let accessor = { get() { return "unchanged"; }, set(value) { calls++; } };

let preparations = {
    "structure is watched": "",
    // The other two leave the receiver with a structure that the DFG does not watch.
    // A structure that has been a dictionary is not watched. Caching the read of an inherited property flattens it.
    "has been a dictionary": `
        for (let i = 0; i < 200; i++)
            receiver["k" + i] = i;
        for (let i = 0; i < 100; i++)
            readMarker();`,
    // Nor is the successor of a structure that changed while compiled code was watching it.
    "changed while watched": `
        for (let i = 0; i < count; i++)
            readMarker();
        receiver.added = 1;`,
};

function test(kind, preparation, strict) {
    let message = [kind, preparation, strict ? "strict" : "sloppy"].join(", ");
    let prototype = Object.create(Object.prototype, { marker: { value: 1 } });
    let receiver = Object.create(prototype);
    if (kind === "data property")
        receiver.property = "initial";
    else
        Object.defineProperty(kind === "own setter" ? receiver : prototype, "property", accessor);
    let prepare = (0, eval)(`(function (receiver, count) {
        let readMarker = () => receiver.marker;
        ${preparations[preparation]}

        let assign = value => { ${strict ? "'use strict';" : ""} receiver.property = value; };
        for (let i = 0; i < count; i++)
            assign("unchanged");
        return assign;
    }) // ${message}`);
    noDFG(prepare); // Its loops would inline what they call, which then would not be compiled on its own.
    calls = 0;
    let assign = prepare(receiver, testLoopCount);
    shouldBe(calls, kind === "data property" ? 0 : testLoopCount, message);

    $vm.makePropertiesImmutable(receiver);
    calls = 0;
    let result = "no exception";
    try {
        assign("changed");
    } catch (error) {
        result = error.constructor.name;
    }
    shouldBe(result, strict ? "TypeError" : "no exception", message);
    shouldBe(calls, 0, message + ", calls of the setter");
    shouldBe(receiver.property, "unchanged", message);
}

for (let kind of ["own setter", "inherited setter", "data property"]) {
    for (let preparation of Object.keys(preparations)) {
        for (let strict of [true, false])
            test(kind, preparation, strict);
    }
}
