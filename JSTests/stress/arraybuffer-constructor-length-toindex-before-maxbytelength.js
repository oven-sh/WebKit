function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`actual: ${actual}, expected: ${expected}`);
}

function shouldThrow(func, errorType, message) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof errorType))
        throw new Error(`Expected ${errorType.name} but got ${error}`);
    if (message !== undefined)
        shouldBe(error.message, message);
}

// https://tc39.es/ecma262/#sec-arraybuffer-length
// https://tc39.es/ecma262/#sec-sharedarraybuffer-length
//   2. Let byteLength be ? ToIndex(length).
//   3. Let requestedMaxByteLength be ? GetArrayBufferMaxByteLengthOption(options).
//   4. Return ? AllocateArrayBuffer(NewTarget, byteLength, requestedMaxByteLength).
// AllocateArrayBuffer / AllocateSharedArrayBuffer
//   3.a. If byteLength > maxByteLength, throw a RangeError exception.
// The comparison is between the two integers that ToIndex produced, not the original length argument.

for (const ArrayBufferConstructor of [ArrayBuffer, SharedArrayBuffer]) {
    const isShared = ArrayBufferConstructor === SharedArrayBuffer;

    function check(length, maxByteLength, expectedByteLength, expectedMaxByteLength)
    {
        const buffer = new ArrayBufferConstructor(length, { maxByteLength });
        shouldBe(buffer.byteLength, expectedByteLength);
        shouldBe(buffer.maxByteLength, expectedMaxByteLength);
        shouldBe(isShared ? buffer.growable : buffer.resizable, true);
    }

    check(1.5, 1, 1, 1);
    check(2.5, 2, 2, 2);
    check(1.9999999, 1, 1, 1);
    check("1.5", 1, 1, 1);
    check("0x10", 16.5, 16, 16);
    check(0.5, 0, 0, 0);
    check(-0.5, 0, 0, 0);
    check(-0, 0, 0, 0);
    check(0.9, 0.1, 0, 0);
    check(8.75, 8.25, 8, 8);
    check({ valueOf() { return 1.7; } }, { valueOf() { return 1.2; } }, 1, 1);
    check(true, 1.5, 1, 1);
    check(null, 0.5, 0, 0);
    check(NaN, NaN, 0, 0);
    check(4.5, 16, 4, 16);

    // Both sides are still compared after truncation.
    shouldThrow(() => new ArrayBufferConstructor(2, { maxByteLength: 1 }), RangeError, "ArrayBuffer length exceeds maxByteLength option");
    shouldThrow(() => new ArrayBufferConstructor(2, { maxByteLength: 1.9 }), RangeError, "ArrayBuffer length exceeds maxByteLength option");
    shouldThrow(() => new ArrayBufferConstructor(1.5, { maxByteLength: 0.9 }), RangeError, "ArrayBuffer length exceeds maxByteLength option");
    shouldThrow(() => new ArrayBufferConstructor("2", { maxByteLength: "1" }), RangeError, "ArrayBuffer length exceeds maxByteLength option");

    // ToIndex(length) completes, including its RangeError, before options.maxByteLength is read and before
    // OrdinaryCreateFromConstructor reads newTarget.prototype.
    {
        const log = [];
        const options = { get maxByteLength() { log.push("maxByteLength"); return 8; } };
        const newTarget = function () { }.bind();
        Object.defineProperty(newTarget, "prototype", { get() { log.push("prototype"); return ArrayBufferConstructor.prototype; } });

        shouldBe(Reflect.construct(ArrayBufferConstructor, [4.5, options], newTarget).byteLength, 4);
        shouldBe(log.join(), "maxByteLength,prototype");

        log.length = 0;
        shouldThrow(() => Reflect.construct(ArrayBufferConstructor, [-1, options], newTarget), RangeError, "length cannot be negative");
        shouldBe(log.join(), "");

        shouldThrow(() => Reflect.construct(ArrayBufferConstructor, [-1.5, options], newTarget), RangeError, "length cannot be negative");
        shouldBe(log.join(), "");

        shouldThrow(() => Reflect.construct(ArrayBufferConstructor, [2 ** 53, options], newTarget), RangeError, "length larger than (2 ** 53) - 1");
        shouldBe(log.join(), "");

        shouldThrow(() => Reflect.construct(ArrayBufferConstructor, [{ valueOf() { throw new SyntaxError("length"); } }, options], newTarget), SyntaxError, "length");
        shouldBe(log.join(), "");

        // byteLength > maxByteLength is checked before newTarget.prototype is read.
        shouldThrow(() => Reflect.construct(ArrayBufferConstructor, [16, options], newTarget), RangeError, "ArrayBuffer length exceeds maxByteLength option");
        shouldBe(log.join(), "maxByteLength");

        // A length that passes ToIndex but cannot be allocated fails after newTarget.prototype is read.
        log.length = 0;
        shouldThrow(() => Reflect.construct(ArrayBufferConstructor, [2 ** 53 - 1], newTarget), RangeError);
        shouldBe(log.join(), "prototype");

        log.length = 0;
        shouldThrow(() => Reflect.construct(ArrayBufferConstructor, [8, { maxByteLength: 2 ** 53 - 1 }], newTarget), RangeError);
        shouldBe(log.join(), "prototype");
    }

    // Without options nothing changes.
    shouldBe(new ArrayBufferConstructor(1.5).byteLength, 1);
    shouldBe(new ArrayBufferConstructor("7").byteLength, 7);
    shouldBe(new ArrayBufferConstructor().byteLength, 0);
    shouldBe(new ArrayBufferConstructor(3, undefined).byteLength, 3);
    shouldBe(new ArrayBufferConstructor(3, 1).byteLength, 3);
    shouldBe(new ArrayBufferConstructor(3.5, { maxByteLength: undefined }).byteLength, 3);
    shouldBe(isShared ? new ArrayBufferConstructor(3.5, {}).growable : new ArrayBufferConstructor(3.5, {}).resizable, false);
    shouldThrow(() => new ArrayBufferConstructor(-1), RangeError, "length cannot be negative");
}
