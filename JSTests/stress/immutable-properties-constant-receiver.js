// An assignment is warmed up on an ordinary object that inherits a setter, then inlined where its receiver is a constant with
// immutable properties and the same prototype. The assignment has never executed on that receiver, and is refused in every tier.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + String(expected) + " but got " + String(actual));
}

let calls = 0;

// Each of these leaves the receiver immutable and with a structure that the DFG does not watch.
let preparations = {
    // A structure that has been a dictionary is not watched. Caching the read of an inherited property flattens it.
    "flattened, then made immutable": `
        for (let i = 0; i < 200; i++)
            receiver["k" + i] = i;
        for (let i = 0; i < 100; i++)
            readMarker();
        $vm.makePropertiesImmutable(receiver);`,
    "made immutable, then flattened": `
        for (let i = 0; i < 200; i++)
            receiver["k" + i] = i;
        $vm.makePropertiesImmutable(receiver);
        for (let i = 0; i < 100; i++)
            readMarker();`,
    // Nor is the successor of a structure that changed while compiled code was watching it. A function's lazy name may still be materialized.
    "made immutable, watched, then given its lazy name": `
        $vm.makePropertiesImmutable(receiver);
        for (let i = 0; i < count; i++)
            readMarker();
        receiver.name;`,
};

function test(makeReceiver, parent, preparation, strict) {
    let message = [makeReceiver, preparation, strict ? "strict" : "sloppy"].join(", ");
    let prototype = Object.create(parent, {
        marker: { value: 1 },
        accessor: { get() { return "inherited"; }, set(value) { calls++; } },
    });
    let receiver = Object.setPrototypeOf(makeReceiver(), prototype);
    let prepare = (0, eval)(`(function (receiver, ordinary, count) {
        let readMarker = () => receiver.marker;
        ${preparations[preparation]}

        let result = "no exception";
        let assign = (object, value) => { ${strict ? "'use strict';" : ""} object.accessor = value; };
        for (let i = 0; i < count; i++)
            assign(ordinary, 1);
        let assignToReceiver = really => {
            if (really)
                assign(receiver, 1);
        };
        for (let i = 0; i < count; i++)
            assignToReceiver(false);
        return () => {
            try {
                assignToReceiver(true);
            } catch (error) {
                result = error.constructor.name;
            }
            return result;
        };
    }) // ${message}`);
    noDFG(prepare); // Its loops would inline what they call, which then would not be compiled on its own.
    let run = prepare(receiver, Object.create(prototype), testLoopCount);
    shouldBe($vm.hasImmutableProperties(receiver), true, message);
    calls = 0;
    shouldBe(run(), strict ? "TypeError" : "no exception", message);
    shouldBe(calls, 0, message + ", calls of the inherited setter");
    shouldBe(receiver.accessor, "inherited", message);
}

for (let strict of [true, false]) {
    for (let preparation of Object.keys(preparations))
        test(() => function () { }, Function.prototype, preparation, strict);
    test(() => ({ }), Object.prototype, "flattened, then made immutable", strict);
    test(() => ({ }), Object.prototype, "made immutable, then flattened", strict);
}
