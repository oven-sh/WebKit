import * as assert from '../assert.js';

// https://webassembly.github.io/spec/js-api/#exceptions
//   any getArg(Tag exceptionTag, [EnforceRange] unsigned long index);
// A WebIDL [EnforceRange] conversion that fails is a TypeError. Only an index that converts and is
// then >= the payload size is a RangeError.

const tag = new WebAssembly.Tag({ parameters: ["i32", "f64"] });
const exception = new WebAssembly.Exception(tag, [5, 6.5]);

const notAnUnsignedLong = [
    -1,
    -1.5,
    2 ** 32,
    2 ** 32 + 0.5,
    2 ** 53,
    Number.MAX_VALUE,
    NaN,
    Infinity,
    -Infinity,
    undefined,
    "x",
    "0x100000000",
    {},
    { valueOf() { return -1; } },
    { valueOf() { return 2 ** 32; } },
];
for (const index of notAnUnsignedLong)
    assert.throws(() => exception.getArg(tag, index), TypeError, "Expect an integer argument in the range: [0, 2^32 - 1]");

// These convert to 0 or 1: ToNumber, -0 becomes +0, then the integer part is taken.
assert.eq(exception.getArg(tag, 0), 5);
assert.eq(exception.getArg(tag, -0), 5);
assert.eq(exception.getArg(tag, -0.5), 5);
assert.eq(exception.getArg(tag, 0.9), 5);
assert.eq(exception.getArg(tag, null), 5);
assert.eq(exception.getArg(tag, false), 5);
assert.eq(exception.getArg(tag, ""), 5);
assert.eq(exception.getArg(tag, 1), 6.5);
assert.eq(exception.getArg(tag, 1.99), 6.5);
assert.eq(exception.getArg(tag, "1"), 6.5);
assert.eq(exception.getArg(tag, true), 6.5);
assert.eq(exception.getArg(tag, [1]), 6.5);

let valueOfCalls = 0;
assert.eq(exception.getArg(tag, { valueOf() { ++valueOfCalls; return 1; } }), 6.5);
assert.eq(valueOfCalls, 1);

// A valid unsigned long past the payload is still a RangeError.
for (const index of [2, 2.5, "2", 2 ** 31, 2 ** 32 - 1])
    assert.throws(() => exception.getArg(tag, index), RangeError, "WebAssembly.Exception.getArg(): Index out of range");

// An empty payload: every index is out of range, but the conversion still comes first.
{
    const emptyTag = new WebAssembly.Tag({ parameters: [] });
    const empty = new WebAssembly.Exception(emptyTag, []);
    assert.throws(() => empty.getArg(emptyTag, 0), RangeError, "WebAssembly.Exception.getArg(): Index out of range");
    assert.throws(() => empty.getArg(emptyTag, -1), TypeError, "Expect an integer argument in the range: [0, 2^32 - 1]");
    assert.throws(() => empty.getArg(emptyTag, undefined), TypeError, "Expect an integer argument in the range: [0, 2^32 - 1]");
}

// The index is converted before the tag is compared with the exception's tag.
{
    const otherTag = new WebAssembly.Tag({ parameters: ["i32", "f64"] });
    assert.throws(() => exception.getArg(otherTag, -1), TypeError, "Expect an integer argument in the range: [0, 2^32 - 1]");
    assert.throws(() => exception.getArg(otherTag, 0), TypeError, "WebAssembly.Exception.getArg(): First argument does not match the exception tag");
}

// ToNumber itself can throw; that exception propagates unchanged.
assert.throws(() => exception.getArg(tag, Symbol()), TypeError, "");
assert.throws(() => exception.getArg(tag, 0n), TypeError, "");
{
    const error = new Error("from valueOf");
    assert.throwsExactly(() => exception.getArg(tag, { valueOf() { throw error; } }), error);
}
